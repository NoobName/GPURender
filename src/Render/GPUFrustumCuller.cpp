#include "Render/GPUFrustumCuller.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "Render/ShaderCompiler.h"

namespace
{
constexpr UINT kFrustumPlaneCount = 6;
constexpr UINT kRootConstantDwords = kFrustumPlaneCount * 4u + 4u; // 6 个 float4 + count + 3 pad

// readback 缓冲里两段数据的偏移，按 256 字节对齐
constexpr UINT64 kReadbackAlignment = 256;

UINT64 AlignUp(UINT64 value, UINT64 alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}
} // namespace

bool GPUFrustumCuller::Initialize(ID3D12Device* device, std::uint32_t maxInstances)
{
    m_capacity = maxInstances;

    // -------------------------------------------------------------------------
    // 1. 根签名：SRV 表 (t0) + UAV 表 (u0, u1) + root constants (b0)
    //
    // u0 = visibleInstanceIndices（压缩列表）
    // u1 = visibleCount（ByteAddressBuffer 计数器）
    // 两者放在**同一张 UAV 表**里，因此描述符必须在堆里相邻。
    // -------------------------------------------------------------------------
    D3D12_ROOT_PARAMETER params[3] = {};

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &srvRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 2; // u0 + u1
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &uavRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 0;
    params[2].Constants.RegisterSpace = 0;
    params[2].Constants.Num32BitValues = kRootConstantDwords;
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
        std::cerr << "[GPUFrustumCuller] D3D12SerializeRootSignature failed\n";
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(),
                                           signature->GetBufferSize(),
                                           IID_PPV_ARGS(&m_rootSignature))))
    {
        std::cerr << "[GPUFrustumCuller] CreateRootSignature failed\n";
        return false;
    }

    // -------------------------------------------------------------------------
    // 2. 编译并创建 Compute PSO
    // -------------------------------------------------------------------------
    std::string compileError;
    std::vector<std::uint8_t> csBytecode = ShaderCompiler::Compile(
        L"FrustumCullingCS.hlsl", L"main", L"cs_6_0", compileError);
    if (csBytecode.empty())
    {
        std::cerr << "[GPUFrustumCuller] Compute shader compile failed:\n"
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
        std::cerr << "[GPUFrustumCuller] CreateComputePipelineState failed\n";
        return false;
    }

    // -------------------------------------------------------------------------
    // 3. READBACK Heap（仅调试验证用，不参与正常渲染）
    // -------------------------------------------------------------------------
    m_countReadbackOffset = 0;
    m_indicesReadbackOffset = AlignUp(sizeof(std::uint32_t), kReadbackAlignment);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = m_indicesReadbackOffset +
                 static_cast<UINT64>(maxInstances) * sizeof(std::uint32_t);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readbackBuffer))))
    {
        std::cerr << "[GPUFrustumCuller] Failed to create readback buffer.\n";
        return false;
    }

    std::cout << "[GPUFrustumCuller] Ready (M11 stream compaction): group size "
              << kThreadGroupSize << ", capacity " << m_capacity
              << " (indices "
              << (static_cast<double>(m_capacity) * sizeof(std::uint32_t) / 1024.0)
              << " KB + 4 B counter)\n";
    return true;
}

void GPUFrustumCuller::Record(ID3D12GraphicsCommandList* cmd,
                              ID3D12DescriptorHeap* descriptorHeap,
                              UINT descriptorSize,
                              UINT instanceSrvSlot,
                              VisibleInstanceList& visibleList,
                              const DirectX::XMFLOAT4 planes[6],
                              std::uint32_t instanceCount)
{
    // ---- 1. 状态转换：两个缓冲都进入 UNORDERED_ACCESS ----
    //
    // **这一步必须排在清零之前。**
    // ClearUnorderedAccessViewUint 不会替调用方做状态转换 ——
    // 它要求目标资源**已经**处于 D3D12_RESOURCE_STATE_UNORDERED_ACCESS。
    // 顺序写反的直接后果是设备移除（Debug 层下就是一次崩溃），
    // 而且因为它发生在 GPU 时间线上，CPU 侧看不到任何有用的报错。
    visibleList.TransitionCountTo(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    visibleList.TransitionIndicesTo(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // ---- 2. 清零计数器 ----
    //
    // 必须在 dispatch 之前、且在**同一条命令列表**里完成：
    // 这样它天然排在本帧的 GPU 执行顺序中，CPU 完全不需要等待或同步。
    //
    // 注意这里用的是**非着色器可见堆**里的 CPU 句柄（第二个参数）——
    // ClearUnorderedAccessViewUint 要求如此；第一个参数是着色器可见堆里
    // 同一份 UAV 的 GPU 句柄，用于让驱动知道「清的是哪个视图」。
    {
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle =
            descriptorHeap->GetGPUDescriptorHandleForHeapStart();
        gpuHandle.ptr += static_cast<UINT64>(visibleList.GetCountUavSlot()) * descriptorSize;

        const UINT clearValue[4] = { 0u, 0u, 0u, 0u };
        cmd->ClearUnorderedAccessViewUint(
            gpuHandle,
            visibleList.GetCountClearCpuHandle(),
            visibleList.GetCountResourceForClear(),
            clearValue, 0, nullptr);
    }

    // ---- 3. UAV barrier：清零 与 随后的原子加 是两次 UAV 写（WAW）----
    //
    // ClearUnorderedAccessViewUint 也是一次 UAV 写入。如果不加这道 barrier，
    // 硬件的命令处理器可能让 dispatch 里某个线程的 InterlockedAdd
    // 先于清零生效 —— 结果是计数器从非零值开始，列表前几项是陈旧数据。
    // 这类错误是间歇性的，且看起来像「偶尔少几个实例」，非常难查。
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = visibleList.GetCountBuffer();
        cmd->ResourceBarrier(1, &barrier);
    }

    // ---- 4. UAV barrier：与上一帧的写入之间（WAW）----
    // 状态转换管「这次访问的类型」，挡不住「上一帧的写还没完成」。
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[0].UAV.pResource = visibleList.GetIndexBuffer();
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[1].UAV.pResource = visibleList.GetCountBuffer();
        cmd->ResourceBarrier(2, barriers);
    }

    cmd->SetComputeRootSignature(m_rootSignature.Get());
    cmd->SetPipelineState(m_pipelineState.Get());

    // 根参数 0：SRV 表 -> StructuredBuffer<InstanceData>
    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    srvHandle.ptr += static_cast<UINT64>(instanceSrvSlot) * descriptorSize;
    cmd->SetComputeRootDescriptorTable(0, srvHandle);

    // 根参数 1：UAV 表 -> u0 索引列表、u1 计数器（两者在堆里必须相邻）
    D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    uavHandle.ptr += static_cast<UINT64>(visibleList.GetIndicesUavSlot()) * descriptorSize;
    cmd->SetComputeRootDescriptorTable(1, uavHandle);

    // 根参数 2：6 个归一化视锥平面 + instanceCount
    std::uint32_t constants[kRootConstantDwords] = {};
    for (UINT i = 0; i < kFrustumPlaneCount; ++i)
    {
        std::memcpy(&constants[i * 4u + 0u], &planes[i].x, sizeof(float));
        std::memcpy(&constants[i * 4u + 1u], &planes[i].y, sizeof(float));
        std::memcpy(&constants[i * 4u + 2u], &planes[i].z, sizeof(float));
        std::memcpy(&constants[i * 4u + 3u], &planes[i].w, sizeof(float));
    }
    constants[kFrustumPlaneCount * 4u] = instanceCount;
    cmd->SetComputeRoot32BitConstants(2, kRootConstantDwords, constants, 0);

    // ---- 5. Dispatch：每 64 个实例一个线程组，向上取整 ----
    cmd->Dispatch(GetGroupCount(instanceCount), 1, 1);

    // ---- 6. UAV barrier：写入 vs 之后任何读取（RAW）----
    //
    // 本帧里紧接着的读取者是 M12 的 ExecuteIndirect（它会读计数器）；
    // 现在是调试读回。无论如何，Pass 写完 UAV 就该有一个 barrier 收尾。
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[0].UAV.pResource = visibleList.GetIndexBuffer();
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[1].UAV.pResource = visibleList.GetCountBuffer();
        cmd->ResourceBarrier(2, barriers);
    }
}

bool GPUFrustumCuller::ReadbackAndCompare(ID3D12Device* device,
                                          ID3D12CommandQueue* queue,
                                          ID3D12Fence* fence,
                                          HANDLE fenceEvent,
                                          UINT64& fenceValue,
                                          VisibleInstanceList& visibleList,
                                          const std::vector<std::uint32_t>& cpuVisibleIndices,
                                          std::uint32_t instanceCount)
{
    if (instanceCount == 0 || instanceCount > m_capacity)
    {
        std::cerr << "[GPUFrustumCuller] instanceCount out of range\n";
        return false;
    }

    // -------------------------------------------------------------------------
    // 1. 记录读回到临时命令列表（这条路径只在按键/参数触发时走，允许阻塞）
    // -------------------------------------------------------------------------
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        std::cerr << "[GPUFrustumCuller] Failed to create command list.\n";
        return false;
    }

    // RAW：保证 CS 的写入对下面的拷贝可见
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[0].UAV.pResource = visibleList.GetIndexBuffer();
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[1].UAV.pResource = visibleList.GetCountBuffer();
        list->ResourceBarrier(2, barriers);
    }

    visibleList.TransitionCountTo(list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    visibleList.TransitionIndicesTo(list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);

    // 先拷计数器，再拷整段索引（最多 maxInstances 个，不看 gpuCount ——
    // 因为 gpuCount 现在还在 GPU 侧，CPU 不知道，这正是「无 CPU 同步」的体现）
    list->CopyBufferRegion(m_readbackBuffer.Get(), m_countReadbackOffset,
                           visibleList.GetCountBuffer(), 0, sizeof(std::uint32_t));
    list->CopyBufferRegion(m_readbackBuffer.Get(), m_indicesReadbackOffset,
                           visibleList.GetIndexBuffer(), 0,
                           static_cast<UINT64>(instanceCount) * sizeof(std::uint32_t));

    visibleList.TransitionIndicesTo(list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    visibleList.TransitionCountTo(list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

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

    // -------------------------------------------------------------------------
    // 2. 读回并校验压缩结果的正确性
    // -------------------------------------------------------------------------
    D3D12_RANGE range = {
        0, static_cast<SIZE_T>(m_indicesReadbackOffset +
                               static_cast<UINT64>(instanceCount) * sizeof(std::uint32_t))
    };
    void* mapped = nullptr;
    if (FAILED(m_readbackBuffer->Map(0, &range, &mapped)))
    {
        std::cerr << "[GPUFrustumCuller] readback map failed\n";
        return false;
    }

    const std::uint8_t* base = static_cast<const std::uint8_t*>(mapped);
    const std::uint32_t gpuCount =
        *reinterpret_cast<const std::uint32_t*>(base + m_countReadbackOffset);
    const std::uint32_t* gpuIndices =
        reinterpret_cast<const std::uint32_t*>(base + m_indicesReadbackOffset);

    // ---- 校验 1：GPU 计数不能超过总实例数（原子加写飞了的直接症状）----
    const bool countInRange = (gpuCount <= instanceCount);

    // ---- 校验 2：列表里不能有越界 ID ----
    std::uint32_t outOfRangeIds = 0;

    // ---- 校验 3：不能有重复（原子返回旧值应当构成 0..K-1 的排列）----
    std::uint32_t duplicateIds = 0;

    // ---- 校验 4：集合必须与 CPU 的可见集完全一致 ----
    std::uint32_t mismatchCount = 0;
    std::vector<std::uint8_t> gpuFlag(instanceCount, 0);
    if (countInRange)
    {
        for (std::uint32_t i = 0; i < gpuCount; ++i)
        {
            const std::uint32_t id = gpuIndices[i];
            if (id >= instanceCount)
            {
                ++outOfRangeIds;
                continue;
            }
            if (gpuFlag[id] != 0)
            {
                ++duplicateIds;
                continue;
            }
            gpuFlag[id] = 1;
        }

        // 与 CPU 的有序索引数组做双指针比对
        std::uint32_t cpuCursor = 0;
        for (std::uint32_t i = 0; i < instanceCount; ++i)
        {
            while (cpuCursor < cpuVisibleIndices.size() && cpuVisibleIndices[cpuCursor] < i)
            {
                ++cpuCursor;
            }
            const bool cpuVisible =
                (cpuCursor < cpuVisibleIndices.size() && cpuVisibleIndices[cpuCursor] == i);
            const bool gpuVisible = (gpuFlag[i] != 0);
            if (cpuVisible != gpuVisible)
            {
                ++mismatchCount;
            }
        }
    }

    m_cpuVisibleCount = static_cast<std::uint32_t>(cpuVisibleIndices.size());
    m_gpuVisibleCount = gpuCount;
    m_mismatchCount = mismatchCount;
    m_hasComparison = true;

    m_readbackBuffer->Unmap(0, nullptr);

    // -------------------------------------------------------------------------
    // 3. 报告
    // -------------------------------------------------------------------------
    const bool ok = countInRange && (outOfRangeIds == 0) && (duplicateIds == 0) &&
                    (mismatchCount == 0) && (gpuCount == m_cpuVisibleCount);

    std::cout << "\n===== GPU Stream Compaction vs CPU Culling (M11) =====\n"
              << "  total instances        : " << instanceCount << "\n"
              << "  CPU visible            : " << m_cpuVisibleCount << "\n"
              << "  GPU visible (counter)  : " << gpuCount << "\n"
              << "  compressed list length : " << gpuCount << "  (was "
              << instanceCount << " boolean flags)\n"
              << "  out-of-range ids       : " << outOfRangeIds << "\n"
              << "  duplicate ids          : " << duplicateIds << "\n"
              << "  set difference vs CPU  : " << mismatchCount << "\n";

    if (ok)
    {
        std::cout << "  -> IDENTICAL (compacted list == CPU visibility set)\n";
    }
    else
    {
        std::cout << "  -> **MISMATCH**\n";
        if (!countInRange)
        {
            std::cout << "     counter exceeded instance count -- atomic 写入越界\n";
        }
        if (outOfRangeIds > 0)
        {
            std::cout << "     list contains ids >= instanceCount -- 压缩下标错乱\n";
        }
        if (duplicateIds > 0)
        {
            std::cout << "     duplicate ids -- 原子返回的旧值出现重复（不应发生）\n";
        }
    }
    std::cout << "===== Compaction check done =====\n\n";

    return ok;
}
