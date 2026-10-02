#include "Render/HZBOcclusionCuller.h"

#include <cstring>
#include <iostream>

#include "Render/ShaderCompiler.h"

namespace
{
constexpr UINT kOcclusionThreadGroupSize = 64; // 与 HZBOcclusionCS.hlsl 的 numthreads 一致
// viewProj(16) + resolution(2) + hzbInvSize(2) + proj00/11(2) + near/far/bias(3)
// + hzbMipCount(1) + candidateCapacity(1) + pad(1) = 28 个 DWORD
constexpr UINT kOcclusionConstantDwords = 28;

bool CreateOcclusionRootSignature(ID3D12Device* device, ID3D12RootSignature** out)
{
    D3D12_ROOT_PARAMETER params[4] = {};

    // 参数 0：root constants（viewProj + 分辨率 + 投影参数 + bias）
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = kOcclusionConstantDwords;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 参数 1：SRV 表（t0 = 实例表，t1 = 候选列表，t2 = HZB）
    // 三者在堆里必须连续 —— 由本类在固定槽位每帧重建视图来保证。
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 4;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 参数 2：UAV 表（u0 = 可见索引，u1 = 可见计数）
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 2;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &uavRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 参数 3：统计 UAV（u2）
    D3D12_DESCRIPTOR_RANGE statsRange = {};
    statsRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    statsRange.NumDescriptors = 1;
    statsRange.BaseShaderRegister = 2;
    statsRange.RegisterSpace = 0;
    statsRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &statsRange;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 4;
    desc.pParameters = params;
    desc.NumStaticSamplers = 0;
    desc.pStaticSamplers = nullptr;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &signature, &error)))
    {
        return false;
    }
    return SUCCEEDED(device->CreateRootSignature(0, signature->GetBufferPointer(),
                                                 signature->GetBufferSize(),
                                                 IID_PPV_ARGS(out)));
}

D3D12_CPU_DESCRIPTOR_HANDLE SlotCpu(ID3D12DescriptorHeap* heap, UINT descriptorSize, UINT slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE SlotGpu(ID3D12DescriptorHeap* heap, UINT descriptorSize, UINT slot)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(slot) * descriptorSize;
    return handle;
}
} // namespace

bool HZBOcclusionCuller::Initialize(ID3D12Device* device,
                                    std::uint32_t maxInstances,
                                    ID3D12DescriptorHeap* descriptorHeap,
                                    UINT descriptorSize,
                                    UINT statsUavSlot,
                                    UINT statsSrvSlot)
{
    m_maxInstances = maxInstances;
    m_statsUavSlot = statsUavSlot;
    m_statsSrvSlot = statsSrvSlot;

    // 可见列表：结构完全复用 M11 的压缩列表。
    // 槽位布局（全部相对 statsUavSlot 顺延，避免与全局规划打架）：
    //   statsUavSlot + 0 : 统计 UAV
    //   statsUavSlot + 1 : 可见索引 UAV
    //   statsUavSlot + 2 : 可见计数 UAV
    //   statsUavSlot + 3 : 可见索引 SRV
    //   statsUavSlot + 4 : 可见计数 SRV
    // （statsSrvSlot 及其后 3 个留给 occlusion CS 的表，见 Record）
    if (!m_visible.Initialize(device, maxInstances, descriptorHeap, descriptorSize,
                              statsUavSlot + 1, statsUavSlot + 2,
                              statsUavSlot + 3, statsUavSlot + 4))
    {
        std::cerr << "[HZBOcclusion] failed to initialize visible list.\n";
        return false;
    }

    // ---- 统计缓冲 ----
    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC statsDesc = {};
    statsDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    statsDesc.Width = kStatsDwords * sizeof(std::uint32_t);
    statsDesc.Height = 1;
    statsDesc.DepthOrArraySize = 1;
    statsDesc.MipLevels = 1;
    statsDesc.SampleDesc.Count = 1;
    statsDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    statsDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &statsDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_stats))))
    {
        std::cerr << "[HZBOcclusion] failed to create stats buffer.\n";
        return false;
    }

    {
        // R32_TYPELESS + RAW UAV：这样同一块 16 字节内存既能当
        // 4 个 uint 的计数器数组，也能被 CopyBufferRegion 整块拷贝。
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = kStatsDwords;
        uavDesc.Buffer.StructureByteStride = 0;
        uavDesc.Buffer.CounterOffsetInBytes = 0;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(m_stats.Get(), nullptr, &uavDesc,
                                          SlotCpu(descriptorHeap, descriptorSize, statsUavSlot));
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = kStatsDwords;
        srvDesc.Buffer.StructureByteStride = 0;
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(m_stats.Get(), &srvDesc,
                                         SlotCpu(descriptorHeap, descriptorSize, statsSrvSlot));
    }

    // ---- 零缓冲（每帧清零统计用）----
    //
    // UPLOAD Heap 上的资源可以直接作为 CopyBufferRegion 的**源**，
    // 所以不需要额外创建 non-shader-visible 描述符堆。
    {
        D3D12_HEAP_PROPERTIES uploadHeap = {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC zeroDesc = statsDesc;
        zeroDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        if (FAILED(device->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &zeroDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_statsZero))))
        {
            std::cerr << "[HZBOcclusion] failed to create stats zero buffer.\n";
            return false;
        }

        // 永久映射后写零。UPLOAD 资源的初值本来就是零，但显式写一次更清楚。
        void* mapped = nullptr;
        if (SUCCEEDED(m_statsZero->Map(0, nullptr, &mapped)))
        {
            std::memset(mapped, 0, kStatsDwords * sizeof(std::uint32_t));
            m_statsZero->Unmap(0, nullptr);
        }
    }

    // ---- READBACK 环形 ----
    {
        D3D12_HEAP_PROPERTIES readbackHeap = {};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC readbackDesc = statsDesc;
        readbackDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        readbackDesc.Width = static_cast<UINT64>(kStatsDwords) * sizeof(std::uint32_t) *
                             kFrameRingSize;

        if (FAILED(device->CreateCommittedResource(
                &readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_statsReadback))))
        {
            std::cerr << "[HZBOcclusion] failed to create stats readback buffer.\n";
            return false;
        }
    }

    // ---- 根签名与 PSO ----
    if (!CreateOcclusionRootSignature(device, &m_rootSignature))
    {
        std::cerr << "[HZBOcclusion] CreateRootSignature failed.\n";
        return false;
    }

    std::string errorMsg;
    std::vector<std::uint8_t> bytecode = ShaderCompiler::Compile(
        L"HZBOcclusionCS.hlsl", L"main", L"cs_6_0", errorMsg);
    if (bytecode.empty())
    {
        std::cerr << "[HZBOcclusion] shader compile failed:\n" << errorMsg << "\n";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.CS = { bytecode.data(), bytecode.size() };
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState))))
    {
        std::cerr << "[HZBOcclusion] CreateComputePipelineState failed.\n";
        return false;
    }

    m_initialized = true;
    std::cout << "[HZBOcclusion] Ready: HZB-based conservative occlusion culling"
              << " (max " << maxInstances << " candidates)\n";
    return true;
}

void HZBOcclusionCuller::Record(ID3D12GraphicsCommandList* cmd,
                                ID3D12Device* device,
                                ID3D12DescriptorHeap* descriptorHeap,
                                UINT descriptorSize,
                                const VisibleInstanceList& candidates,
                                ID3D12Resource* instanceBuffer,
                                ID3D12Resource* hzbResource,
                                UINT hzbMipCount,
                                UINT hzbMip0Width,
                                UINT hzbMip0Height,
                                const DirectX::XMFLOAT4X4& viewProj,
                                float proj00,
                                float proj11,
                                UINT width,
                                UINT height,
                                float nearPlane,
                                float farPlane,
                                float depthBias)
{
    if (!m_initialized)
    {
        return;
    }

    // ---- 1. 清零输出计数与统计 ----
    //
    // 顺序很重要：**必须先转到 UNORDERED_ACCESS，再调用 clear**。
    // ClearUnorderedAccessViewUint 不会替你做状态转换（M11 在这里踩过设备移除）。
    m_visible.TransitionIndicesTo(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_visible.TransitionCountTo(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    {
        const UINT clearValues[4] = { 0u, 0u, 0u, 0u };
        cmd->ClearUnorderedAccessViewUint(
            SlotGpu(descriptorHeap, descriptorSize, m_visible.GetCountUavSlot()),
            m_visible.GetCountClearCpuHandle(),
            m_visible.GetCountResourceForClear(),
            clearValues, 0, nullptr);
    }

    // 统计清零：从 UPLOAD 零缓冲拷过去。
    // 不清零的话 InterlockedAdd 会让数字逐帧累加。
    {
        // COMMON -> COPY_DEST
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_stats.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cmd->ResourceBarrier(1, &barrier);

        cmd->CopyBufferRegion(m_stats.Get(), 0, m_statsZero.Get(), 0,
                              kStatsDwords * sizeof(std::uint32_t));

        // COPY_DEST -> UNORDERED_ACCESS
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cmd->ResourceBarrier(1, &barrier);
    }

    // ---- 2. 重建 occlusion CS 自己的连续 SRV 表 ----
    //
    // 描述符表只能取堆里的一段连续区间，而「实例表 / 候选列表 / HZB」
    // 在全局槽位规划里相距很远。所以在固定槽位重建这三张视图 ——
    // CreateShaderResourceView 很廉价，换来的是槽位规划完全解耦。
    const UINT srvBase = GetOcclusionSrvSlot();

    {
        // t0：实例表（StructuredBuffer<InstanceData>，stride 96）
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Buffer.FirstElement = 0;
        desc.Buffer.NumElements = m_maxInstances;
        desc.Buffer.StructureByteStride = 96; // sizeof(InstanceData)
        desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        device->CreateShaderResourceView(instanceBuffer, &desc,
                                         SlotCpu(descriptorHeap, descriptorSize, srvBase + 0));
    }

    {
        // t1：候选索引列表（StructuredBuffer<uint>，stride 4）
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Buffer.FirstElement = 0;
        desc.Buffer.NumElements = candidates.GetCapacity();
        desc.Buffer.StructureByteStride = 4;
        desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        device->CreateShaderResourceView(candidates.GetIndexBuffer(), &desc,
                                         SlotCpu(descriptorHeap, descriptorSize, srvBase + 1));
    }

    {
        // t3：候选计数器（RAW ByteAddressBuffer，4 字节）
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Buffer.FirstElement = 0;
        desc.Buffer.NumElements = 1;
        desc.Buffer.StructureByteStride = 0;
        desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(candidates.GetCountBuffer(), &desc,
                                         SlotCpu(descriptorHeap, descriptorSize, srvBase + 3));
    }

    // t2：HZB（Texture2D，完整 mip 链）
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MostDetailedMip = 0;
        desc.Texture2D.MipLevels = hzbMipCount;
        desc.Texture2D.PlaneSlice = 0;
        desc.Texture2D.ResourceMinLODClamp = 0.0f;
        device->CreateShaderResourceView(hzbResource, &desc,
                                         SlotCpu(descriptorHeap, descriptorSize, srvBase + 2));
    }

    // ---- 3. 绑定并 Dispatch ----
    cmd->SetComputeRootSignature(m_rootSignature.Get());
    cmd->SetPipelineState(m_pipelineState.Get());

    struct Constants
    {
        DirectX::XMFLOAT4X4 viewProj; // 16 DWORD
        float resolution[2];          //  2
        float hzbInvSize[2];          //  2
        float proj00;                 //  1
        float proj11;                 //  1
        float nearPlane;              //  1
        float farPlane;               //  1
        float depthBias;              //  1
        std::uint32_t hzbMipCount;    //  1
        std::uint32_t candidateCapacity; // 1（实际数量由 GPU 侧的计数器决定）
        std::uint32_t pad0;           //  1
    };
    static_assert(sizeof(Constants) == kOcclusionConstantDwords * sizeof(std::uint32_t),
                  "HZBOcclusionCS root constants layout mismatch");

    Constants constants = {};
    constants.viewProj = viewProj;
    constants.resolution[0] = static_cast<float>(width);
    constants.resolution[1] = static_cast<float>(height);
    constants.hzbInvSize[0] = 1.0f / static_cast<float>(hzbMip0Width);
    constants.hzbInvSize[1] = 1.0f / static_cast<float>(hzbMip0Height);
    constants.proj00 = proj00;
    constants.proj11 = proj11;
    constants.nearPlane = nearPlane;
    constants.farPlane = farPlane;
    constants.depthBias = depthBias;
    constants.hzbMipCount = hzbMipCount;
    // dispatch 规模按**容量**取整（CPU 不知道实际候选数，也不该去读它）。
    // 多出来的线程读到 gCandidateCount 后自然退出。
    constants.candidateCapacity = candidates.GetCapacity();

    cmd->SetComputeRoot32BitConstants(0, kOcclusionConstantDwords, &constants, 0);
    cmd->SetComputeRootDescriptorTable(1, SlotGpu(descriptorHeap, descriptorSize, srvBase));
    cmd->SetComputeRootDescriptorTable(
        2, SlotGpu(descriptorHeap, descriptorSize, m_visible.GetIndicesUavSlot()));
    cmd->SetComputeRootDescriptorTable(
        3, SlotGpu(descriptorHeap, descriptorSize, m_statsUavSlot));

    cmd->Dispatch((candidates.GetCapacity() + kOcclusionThreadGroupSize - 1u) /
                      kOcclusionThreadGroupSize,
                  1, 1);

    // 让下游（命令生成 / Main Pass）看到写入
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = m_visible.GetIndexBuffer();
        cmd->ResourceBarrier(1, &barrier);
    }
}

void HZBOcclusionCuller::ResolveStats(ID3D12GraphicsCommandList* cmd, UINT frameIndex)
{
    if (!m_initialized || frameIndex >= kFrameRingSize || m_stats == nullptr)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_stats.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->ResourceBarrier(1, &barrier);

    cmd->CopyBufferRegion(m_statsReadback.Get(),
                          static_cast<UINT64>(frameIndex) * kStatsDwords * sizeof(std::uint32_t),
                          m_stats.Get(), 0,
                          kStatsDwords * sizeof(std::uint32_t));

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cmd->ResourceBarrier(1, &barrier);
}

HZBOcclusionCuller::FrameStats HZBOcclusionCuller::ReadbackStats(UINT laggedFrameIndex) const
{
    FrameStats result;
    if (!m_initialized || laggedFrameIndex >= kFrameRingSize || m_statsReadback == nullptr)
    {
        return result;
    }

    const UINT64 offset = static_cast<UINT64>(laggedFrameIndex) *
                          kStatsDwords * sizeof(std::uint32_t);

    const D3D12_RANGE range = { static_cast<SIZE_T>(offset),
                                static_cast<SIZE_T>(offset +
                                    kStatsDwords * sizeof(std::uint32_t)) };
    void* mapped = nullptr;
    if (FAILED(m_statsReadback->Map(0, &range, &mapped)))
    {
        return result;
    }

    const std::uint32_t* values = reinterpret_cast<const std::uint32_t*>(
        static_cast<const std::uint8_t*>(mapped) + offset);

    result.candidateCount = values[kStatCandidateCount];
    result.occludedCount = values[kStatOccludedCount];
    result.conservativePassCount = values[kStatConservativePassCount];
    result.sampledTexels = values[kStatSampledTexels];
    result.valid = true;

    m_statsReadback->Unmap(0, nullptr);
    return result;
}
