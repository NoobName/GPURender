#include "Render/IndirectDrawCommands.h"

#include <iostream>

#include "Render/ShaderCompiler.h"

namespace
{
// root constants：{ lodCount, pad0, pad1, pad2 }
constexpr UINT kCommandConstantDwords = 4;

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
                                      std::uint32_t lodCount,
                                      ID3D12DescriptorHeap* descriptorHeap,
                                      UINT descriptorSize,
                                      UINT argumentsUavSlot)
{
    if (lodCount == 0)
    {
        std::cerr << "[IndirectDrawCommands] lodCount must be > 0\n";
        return false;
    }

    m_maxInstances = maxInstances;
    m_lodCount = lodCount;
    m_argumentsUavSlot = argumentsUavSlot;

    // -------------------------------------------------------------------------
    // 1. Indirect Argument Buffer
    //
    // 容量 = LOD 级数（每个 LOD 一条命令），不再是「最大可见实例数」。
    // 4 级 -> 4 * 20 B = 80 字节。这是 M16 相对 M12 的一个显著收益：
    // 命令缓冲与可见数**彻底解耦**了。
    // -------------------------------------------------------------------------
    const UINT64 argumentBytes = static_cast<UINT64>(lodCount) * kDrawIndexedArgumentSize;
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
    uavDesc.Buffer.NumElements = lodCount * (kDrawIndexedArgumentSize / 4u);
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
    // 3. 命令生成 Compute Pass 的根签名（M16：4 个参数）
    //
    // 参数 0：32-bit root constants (b0) = { lodCount, pad, pad, pad }
    // 参数 1：SRV 表 (t0) = { LOD 元数据 }
    // 参数 2：SRV 表 (t1) = { 每级实例计数器 }
    // 参数 3：UAV 表 (u0) = { indirect argument buffer }
    //
    // 为什么把两个 SRV 拆成两张表而不是一张：描述符表只能取堆里**连续**区间，
    // 而 LOD 元数据与可见列表的计数器在全局槽位规划里相距很远。
    // 拆成两张各 1 个描述符的表，就完全不受槽位布局约束。
    // -------------------------------------------------------------------------
    D3D12_ROOT_PARAMETER params[4] = {};

    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = kCommandConstantDwords;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE lodRange = {};
    lodRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    lodRange.NumDescriptors = 1; // t0
    lodRange.BaseShaderRegister = 0;
    lodRange.RegisterSpace = 0;
    lodRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &lodRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE countRange = {};
    countRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    countRange.NumDescriptors = 1; // t1
    countRange.BaseShaderRegister = 1;
    countRange.RegisterSpace = 0;
    countRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &countRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1; // u0
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &uavRange;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = 4;
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

    std::cout << "[IndirectDrawCommands] Ready: " << m_lodCount
              << " LOD commands x " << kDrawIndexedArgumentSize << " B = "
              << argumentBytes << " B argument buffer"
              << " (max " << m_maxInstances << " instances tracked)\n";
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
                                  UINT lodMetadataSrvSlot)
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

    // 根参数 0：{ lodCount, 0, 0, 0 }
    const std::uint32_t constants[kCommandConstantDwords] = {
        m_lodCount, 0u, 0u, 0u
    };
    cmd->SetComputeRoot32BitConstants(0, kCommandConstantDwords, constants, 0);

    // 根参数 1：SRV 表 -> t0 LOD 元数据
    D3D12_GPU_DESCRIPTOR_HANDLE lodHandle = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    lodHandle.ptr += static_cast<UINT64>(lodMetadataSrvSlot) * descriptorSize;
    cmd->SetComputeRootDescriptorTable(1, lodHandle);

    // 根参数 2：SRV 表 -> t1 每级实例计数器
    D3D12_GPU_DESCRIPTOR_HANDLE countHandle = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    countHandle.ptr += static_cast<UINT64>(visibleList.GetCountSrvSlot()) * descriptorSize;
    cmd->SetComputeRootDescriptorTable(2, countHandle);

    // 根参数 3：UAV 表 -> u0 参数缓冲
    D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    uavHandle.ptr += static_cast<UINT64>(m_argumentsUavSlot) * descriptorSize;
    cmd->SetComputeRootDescriptorTable(3, uavHandle);

    // ---- 5. Dispatch ----
    //
    // 一个线程写一条命令（一个 LOD），所以组数按 LOD 级数取整。
    // **注意**：块大小是 8（见 GenerateDrawCommandsCS.hlsl 的 numthreads）。
    // 实际的 InstanceCount 仍然完全由 GPU 决定，CPU 不知道也不需要知道。
    constexpr UINT kCommandThreadGroupSize = 8u;
    const UINT groupCount = (m_lodCount + kCommandThreadGroupSize - 1u) / kCommandThreadGroupSize;
    cmd->Dispatch(groupCount, 1, 1);

    // ---- 6. UAV barrier：写入 vs 之后 ExecuteIndirect 的读取（RAW）----
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = m_arguments.Get();
        cmd->ResourceBarrier(1, &barrier);
    }
}
