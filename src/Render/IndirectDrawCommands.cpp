#include "Render/IndirectDrawCommands.h"

#include <iostream>

#include "Render/ShaderCompiler.h"

namespace
{
constexpr UINT kCommandConstantDwords = 4; // maxCommands / indexCount / pad / pad

bool CreateUavBuffer(ID3D12Device* device, UINT64 sizeBytes, ID3D12Resource** out)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // Buffer 的 InitialState 会被运行时忽略，一律写 COMMON（否则 Debug Layer 报 ID=1328）
    return SUCCEEDED(device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(out)));
}
} // namespace

bool IndirectDrawCommands::Initialize(ID3D12Device* device,
                                      std::uint32_t maxInstances,
                                      ID3D12DescriptorHeap* descriptorHeap,
                                      UINT descriptorSize,
                                      UINT argumentsUavSlot,
                                      UINT visibleIndicesSrvSlot,
                                      UINT visibleCountSrvSlot)
{
    m_capacity = maxInstances;
    m_argumentsUavSlot = argumentsUavSlot;
    m_visibleIndicesSrvSlot = visibleIndicesSrvSlot;
    m_visibleCountSrvSlot = visibleCountSrvSlot;

    // -------------------------------------------------------------------------
    // 1. Indirect Argument Buffer
    //
    // 容量按最大实例数：最坏情况「全部可见」，就是 N 条命令。
    // 100k 实例 -> 100000 * 20 B = 2 MB。
    // -------------------------------------------------------------------------
    const UINT64 argumentBytes =
        static_cast<UINT64>(maxInstances) * kDrawIndexedArgumentSize;
    if (!CreateUavBuffer(device, argumentBytes, &m_arguments))
    {
        std::cerr << "[IndirectDrawCommands] Failed to create argument buffer.\n";
        return false;
    }

    // UAV 描述符用 RAW 视图（R32_UINT），因为命令是手工按字节偏移写的。
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R32_UINT;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = maxInstances * (kDrawIndexedArgumentSize / 4u);
    uavDesc.Buffer.StructureByteStride = 0; // RAW 视图
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    uavHandle.ptr += static_cast<SIZE_T>(argumentsUavSlot) * descriptorSize;
    device->CreateUnorderedAccessView(m_arguments.Get(), nullptr, &uavDesc, uavHandle);

    // -------------------------------------------------------------------------
    // 2. Command Signature
    //
    // 它是「参数缓冲 -> 命令」的解释器。没有它，ExecuteIndirect 拿到的
    // 只是一块内存；有了它，驱动才知道每个 ByteStride 字节要当作
    // 一条 DRAW_INDEXED 命令来解析。
    //
    // pRootSignature 传 nullptr：DRAW_INDEXED 这类命令不携带根参数，
    // 只有 DISPATCH 或带常量的自定义签名才需要根签名。
    // -------------------------------------------------------------------------
    D3D12_INDIRECT_ARGUMENT_DESC argumentDesc = {};
    argumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    D3D12_COMMAND_SIGNATURE_DESC signatureDesc = {};
    signatureDesc.ByteStride = kDrawIndexedArgumentSize;
    signatureDesc.NumArgumentDescs = 1;
    signatureDesc.pArgumentDescs = &argumentDesc;
    signatureDesc.NodeMask = 0;

    if (FAILED(device->CreateCommandSignature(&signatureDesc, nullptr,
                                              IID_PPV_ARGS(&m_commandSignature))))
    {
        std::cerr << "[IndirectDrawCommands] CreateCommandSignature failed.\n";
        return false;
    }

    // -------------------------------------------------------------------------
    // 3. 命令生成 Compute Pass 的根签名
    //
    // 参数 0：32-bit root constants (b0) = { maxCommands, indexCount, pad, pad }
    // 参数 1：SRV 表 (t0, t1) = { 可见索引列表, 可见计数器 }
    // 参数 2：UAV 表 (u0)     = { indirect argument buffer }
    //
    // 注意 t0/t1 用 ByteAddressBuffer 绑定，而这两个资源在 M11 里是以
    // RWByteAddressBuffer / RWStructuredBuffer 的 UAV 形式创建的 ——
    // 同一个资源可以同时有 SRV 与 UAV 视图，这里读它们用的是 **SRV 视图**
    // （只读访问能走只读缓存路径，比用 UAV 读更合适）。
    // -------------------------------------------------------------------------
    D3D12_ROOT_PARAMETER params[3] = {};

    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = kCommandConstantDwords;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 2; // t0 + t1
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1; // u0
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &uavRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = 3;
    rootDesc.pParameters = params;
    rootDesc.NumStaticSamplers = 0;
    rootDesc.pStaticSamplers = nullptr;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &signature, &error)))
    {
        std::cerr << "[IndirectDrawCommands] D3D12SerializeRootSignature failed\n";
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(),
                                           signature->GetBufferSize(),
                                           IID_PPV_ARGS(&m_rootSignature))))
    {
        std::cerr << "[IndirectDrawCommands] CreateRootSignature failed\n";
        return false;
    }

    // -------------------------------------------------------------------------
    // 4. 编译并创建 Compute PSO
    // -------------------------------------------------------------------------
    std::string compileError;
    std::vector<std::uint8_t> csBytecode = ShaderCompiler::Compile(
        L"GenerateDrawCommandsCS.hlsl", L"main", L"cs_6_0", compileError);
    if (csBytecode.empty())
    {
        std::cerr << "[IndirectDrawCommands] Compute shader compile failed:\n"
                  << compileError << "\n";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.CS = { csBytecode.data(), csBytecode.size() };
    psoDesc.NodeMask = 0;
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState))))
    {
        std::cerr << "[IndirectDrawCommands] CreateComputePipelineState failed\n";
        return false;
    }

    std::cout << "[IndirectDrawCommands] Ready: capacity " << m_capacity
              << " commands x " << kDrawIndexedArgumentSize << " B = "
              << (argumentBytes / 1024.0 / 1024.0) << " MB argument buffer\n";
    return true;
}

bool IndirectDrawCommands::DebugReadbackFirstCommands(ID3D12Device* device,
                                                      ID3D12CommandQueue* queue,
                                                      ID3D12Fence* fence,
                                                      HANDLE fenceEvent,
                                                      UINT64& fenceValue,
                                                      UINT commandCount)
{
    const UINT64 byteCount = static_cast<UINT64>(commandCount) * kDrawIndexedArgumentSize;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = byteCount;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback))))
    {
        return false;
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        return false;
    }

    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = m_arguments.Get();
        list->ResourceBarrier(1, &barrier);
    }

    TransitionTo(list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, m_arguments.Get(), 0, byteCount);
    TransitionTo(list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    list->Close();
    ID3D12CommandList* const lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);

    ++fenceValue;
    queue->Signal(fence, fenceValue);
    if (fence->GetCompletedValue() < fenceValue)
    {
        fence->SetEventOnCompletion(fenceValue, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }

    D3D12_RANGE range = { 0, static_cast<SIZE_T>(byteCount) };
    void* mapped = nullptr;
    if (FAILED(readback->Map(0, &range, &mapped)))
    {
        return false;
    }
    const std::uint32_t* words = static_cast<const std::uint32_t*>(mapped);

    std::cout << "[IndirectArgs] first " << commandCount << " commands:\n";
    for (UINT i = 0; i < commandCount; ++i)
    {
        const std::uint32_t* c = words + i * (kDrawIndexedArgumentSize / 4u);
        std::cout << "   cmd[" << i << "] indexCount=" << c[0] << " instanceCount=" << c[1]
                  << " startIndex=" << c[2] << " baseVertex=" << static_cast<std::int32_t>(c[3])
                  << " startInstance=" << c[4] << "\n";
    }
    readback->Unmap(0, nullptr);
    return true;
}

void IndirectDrawCommands::TransitionTo(ID3D12GraphicsCommandList* cmd,
                                        D3D12_RESOURCE_STATES newState){
    if (m_state == newState || m_arguments == nullptr)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_arguments.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = m_state;
    barrier.Transition.StateAfter = newState;
    cmd->ResourceBarrier(1, &barrier);

    m_state = newState;
}

void IndirectDrawCommands::Record(ID3D12GraphicsCommandList* cmd,
                                  ID3D12DescriptorHeap* descriptorHeap,
                                  UINT descriptorSize,
                                  VisibleInstanceList& visibleList,
                                  UINT indexCount)
{
    // ---- 1. UAV barrier：保证 M11 的压缩结果对本次读取可见（RAW）----
    //
    // 压缩 Pass 与本 Pass 在同一条命令列表里相邻，但「相邻」不等于「有序」：
    // 两者都是 UAV 访问，硬件不保证完成顺序，必须显式插 barrier。
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[0].UAV.pResource = visibleList.GetIndexBuffer();
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[1].UAV.pResource = visibleList.GetCountBuffer();
        cmd->ResourceBarrier(2, barriers);
    }

    // ---- 2. 状态转换：被读取的两个缓冲离开 UNORDERED_ACCESS ----
    //
    // **这一步不能省。** 本 Pass 用 **SRV 表**绑定它们（只读），
    // 而 M11 的剔除 CS 把它们留在了 UNORDERED_ACCESS 状态。
    // 资源状态与「绑定视图的类型」必须匹配，否则是未定义行为
    //（Debug 层下通常表现为设备移除）。
    visibleList.TransitionIndicesTo(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    visibleList.TransitionCountTo(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // ---- 3. 参数缓冲进入 UNORDERED_ACCESS（本次访问类型：CS 写）----
    TransitionTo(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // ---- 4. UAV barrier：与上一帧的写入之间（WAW）----
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = m_arguments.Get();
        cmd->ResourceBarrier(1, &barrier);
    }

    cmd->SetComputeRootSignature(m_rootSignature.Get());
    cmd->SetPipelineState(m_pipelineState.Get());

    // 根参数 0：{ maxCommands, indexCount, 0, 0 }
    const std::uint32_t constants[kCommandConstantDwords] = {
        m_capacity, indexCount, 0u, 0u
    };
    cmd->SetComputeRoot32BitConstants(0, kCommandConstantDwords, constants, 0);

    // 根参数 1：SRV 表 -> t0 可见索引、t1 可见计数
    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    srvHandle.ptr += static_cast<UINT64>(m_visibleIndicesSrvSlot) * descriptorSize;
    cmd->SetComputeRootDescriptorTable(1, srvHandle);

    // 根参数 2：UAV 表 -> u0 参数缓冲
    D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    uavHandle.ptr += static_cast<UINT64>(m_argumentsUavSlot) * descriptorSize;
    cmd->SetComputeRootDescriptorTable(2, uavHandle);

    // ---- 5. Dispatch ----
    //
    // 线程组数按**容量上限**取整，而不是按可见数 —— 因为 CPU 根本不知道可见数。
    // 这是 GPU-Driven 的一个直接体现：dispatch 规模是固定的，
    // 实际做多少事由 GPU 内部的数据决定。
    const UINT groupCount = (m_capacity + 63u) / 64u;
    cmd->Dispatch(groupCount, 1, 1);

    // ---- 6. UAV barrier：写入 vs 之后 ExecuteIndirect 的读取（RAW）----
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = m_arguments.Get();
        cmd->ResourceBarrier(1, &barrier);
    }
}
