#include "Render/InstanceValidator.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>

#include "Render/ShaderCompiler.h"

namespace
{
// readback 内各段的偏移都要按 256 字节对齐：
// 这是 CopyBufferRegion 最稳妥的对齐要求（4 字节是下限，256 是通用做法）。
constexpr UINT64 kReadbackAlignment = 256;

UINT64 AlignUp(UINT64 value, UINT64 alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}
} // namespace

bool InstanceValidator::Initialize(ID3D12Device* device,
                                   std::uint32_t maxInstances,
                                   ID3D12DescriptorHeap* descriptorHeap,
                                   UINT descriptorSize,
                                   UINT resultsUavSlot,
                                   UINT dumpUavSlot)
{
    m_capacity = maxInstances;

    // -------------------------------------------------------------------------
    // 1. 根签名：SRV 表 (t0) + UAV 表 (u0,u1) + 32-bit root constants (b0)
    //
    // 这里刻意不复用主渲染的根签名：验证是离线的、一次性的流程，
    // 用一套独立且最小的绑定反而更清楚，也不会污染主路径的根签名布局。
    // -------------------------------------------------------------------------
    D3D12_ROOT_PARAMETER params[3] = {};

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0; // t0 = StructuredBuffer<InstanceData>
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &srvRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 2;
    uavRange.BaseShaderRegister = 0; // u0 = results, u1 = dump
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &uavRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 0;
    params[2].Constants.RegisterSpace = 0;
    params[2].Constants.Num32BitValues = 4; // instanceCount / dumpCount / pad / pad
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = 3;
    rootDesc.pParameters = params;
    rootDesc.NumStaticSamplers = 0;
    rootDesc.pStaticSamplers = nullptr;
    // Compute Shader 不需要顶点输入，因此不设 ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &signature, &error)))
    {
        std::cerr << "[InstanceValidator] D3D12SerializeRootSignature failed\n";
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(),
                                           signature->GetBufferSize(),
                                           IID_PPV_ARGS(&m_rootSignature))))
    {
        std::cerr << "[InstanceValidator] CreateRootSignature failed\n";
        return false;
    }

    // -------------------------------------------------------------------------
    // 2. 编译并创建 Compute PSO
    // -------------------------------------------------------------------------
    std::string compileError;
    std::vector<std::uint8_t> csBytecode = ShaderCompiler::Compile(
        L"InstanceValidationCS.hlsl", L"main", L"cs_6_0", compileError);
    if (csBytecode.empty())
    {
        std::cerr << "[InstanceValidator] Compute shader compile failed:\n"
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
        std::cerr << "[InstanceValidator] CreateComputePipelineState failed\n";
        return false;
    }

    // -------------------------------------------------------------------------
    // 3. 创建 UAV 缓冲（DEFAULT Heap）
    //
    // 注意：UPLOAD Heap 的资源**不能**作为 UAV，这是 D3D12 的硬性限制，
    // 所以必须用 DEFAULT Heap + 回读到 READBACK Heap。
    // -------------------------------------------------------------------------
    const UINT64 resultsBytes = static_cast<UINT64>(m_capacity) * sizeof(std::uint32_t);

    // dump 缓冲的元素是 float4，所以这里必须用「float4 的个数」而不是「float 的个数」。
    // 单位混用会让缓冲比 CS 需要的少 16 字节，而 CS 写越界时整个 dispatch 会被丢弃
    // （表现为「读回来全是 0」，非常难查）。
    const UINT64 dumpElementCount =
        kDumpInfoFloats + static_cast<UINT64>(kMaxDumpInstances) * kFloatsPerInstance;
    const UINT64 dumpBytes = dumpElementCount * sizeof(float) * 4u;

    auto createBuffer = [device](UINT64 sizeBytes, D3D12_RESOURCE_FLAGS flags,
                                 D3D12_RESOURCE_STATES initialState,
                                 ID3D12Resource** out) -> bool
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
        desc.Flags = flags;

        return SUCCEEDED(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
            IID_PPV_ARGS(out)));
    };

    if (!createBuffer(resultsBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COMMON, &m_resultsBuffer))
    {
        std::cerr << "[InstanceValidator] Failed to create results buffer.\n";
        return false;
    }
    if (!createBuffer(dumpBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COMMON, &m_dumpBuffer))
    {
        std::cerr << "[InstanceValidator] Failed to create dump buffer.\n";
        return false;
    }

    // READBACK Heap 的缓冲天然处于 COPY_DEST 状态
    m_resultsOffset = 0;
    m_dumpOffset = AlignUp(resultsBytes, kReadbackAlignment);
    m_resultsBytes = resultsBytes;
    m_dumpBytes = dumpBytes;

    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = m_dumpOffset + m_dumpBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readbackBuffer))))
        {
            std::cerr << "[InstanceValidator] Failed to create readback buffer.\n";
            return false;
        }
    }

    // -------------------------------------------------------------------------
    // 4. 在 shader-visible 堆里创建两个 UAV 描述符
    //
    // 根签名把它们放在**同一张描述符表**里（u0, u1），因此两者必须在堆里相邻：
    // 表的 GPU 句柄只指向第一项，硬件按 Range 顺序往后取。
    // 这里显式检查这个前提，避免以后有人改了槽位定义却忘了这个约束。
    // -------------------------------------------------------------------------
    if (dumpUavSlot != resultsUavSlot + 1u)
    {
        std::cerr << "[InstanceValidator] dumpUavSlot must immediately follow "
                     "resultsUavSlot (they share one descriptor table)\n";
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE resultsHandle =
        descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    resultsHandle.ptr += static_cast<SIZE_T>(resultsUavSlot) * descriptorSize;

    D3D12_CPU_DESCRIPTOR_HANDLE dumpHandle =
        descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    dumpHandle.ptr += static_cast<SIZE_T>(dumpUavSlot) * descriptorSize;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN; // StructuredBuffer 必须用 UNKNOWN
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = m_capacity;
    uavDesc.Buffer.StructureByteStride = sizeof(std::uint32_t);
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    device->CreateUnorderedAccessView(m_resultsBuffer.Get(), nullptr, &uavDesc, resultsHandle);

    uavDesc.Buffer.NumElements = static_cast<UINT>(dumpElementCount);
    uavDesc.Buffer.StructureByteStride = sizeof(float) * 4u; // float4
    device->CreateUnorderedAccessView(m_dumpBuffer.Get(), nullptr, &uavDesc, dumpHandle);

    return true;
}

void InstanceValidator::Record(ID3D12GraphicsCommandList* cmd,
                               ID3D12DescriptorHeap* descriptorHeap,
                               UINT descriptorSize,
                               UINT instanceSrvSlot,
                               UINT resultsUavSlot,
                               std::uint32_t instanceCount)
{
    const std::uint32_t dumpCount = std::min(instanceCount, kMaxDumpInstances);

    // ---- 1. UAV 缓冲就绪：COMMON -> UNORDERED_ACCESS ----
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = m_resultsBuffer.Get();
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = m_dumpBuffer.Get();

        cmd->ResourceBarrier(2, barriers);
    }

    // ---- 2. 绑定并 dispatch ----
    // 调用方必须已经调用过 SetDescriptorHeaps，否则下面这些堆内句柄无法被解释。
    cmd->SetComputeRootSignature(m_rootSignature.Get());
    cmd->SetPipelineState(m_pipelineState.Get());

    // 根参数 0：SRV 表，指向 StructuredBuffer<InstanceData>
    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    srvHandle.ptr += static_cast<UINT64>(instanceSrvSlot) * descriptorSize;
    cmd->SetComputeRootDescriptorTable(0, srvHandle);

    // 根参数 1：UAV 表（results 与 dump 必须在堆里连续，表才能一次覆盖两个）
    D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    uavHandle.ptr += static_cast<UINT64>(resultsUavSlot) * descriptorSize;
    cmd->SetComputeRootDescriptorTable(1, uavHandle);

    // 根参数 2：32-bit root constants = { instanceCount, dumpCount, 0, 0 }
    const std::uint32_t constants[4] = { instanceCount, dumpCount, 0u, 0u };
    cmd->SetComputeRoot32BitConstants(2, 4, constants, 0);

    // 每线程处理一个实例，线程组 64
    const UINT groupCount = (instanceCount + 63u) / 64u;
    cmd->Dispatch(groupCount, 1, 1);

    // ---- 3. UAV barrier：保证 CS 的写入对后续 copy 可见 ----
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[0].UAV.pResource = m_resultsBuffer.Get();
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[1].UAV.pResource = m_dumpBuffer.Get();
        cmd->ResourceBarrier(2, barriers);
    }

    // ---- 4. UNORDERED_ACCESS -> COPY_SOURCE，然后拷进 readback ----
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = m_resultsBuffer.Get();
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = m_dumpBuffer.Get();

        cmd->ResourceBarrier(2, barriers);
    }

    cmd->CopyBufferRegion(m_readbackBuffer.Get(), m_resultsOffset,
                          m_resultsBuffer.Get(), 0, m_resultsBytes);
    cmd->CopyBufferRegion(m_readbackBuffer.Get(), m_dumpOffset,
                          m_dumpBuffer.Get(), 0, m_dumpBytes);

    // ---- 5. 回到 COMMON，让下一次验证可以从同样的起点开始 ----
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = m_resultsBuffer.Get();
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;

        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = m_dumpBuffer.Get();

        cmd->ResourceBarrier(2, barriers);
    }

    m_hasResult = true;
}

bool InstanceValidator::Report(const std::vector<InstanceData>& cpuInstances)
{
    if (!m_hasResult)
    {
        std::cout << "[Validation] no result available (Record was never called)\n";
        return false;
    }

    D3D12_RANGE range = { 0, static_cast<SIZE_T>(m_dumpOffset + m_dumpBytes) };
    void* mapped = nullptr;
    if (FAILED(m_readbackBuffer->Map(0, &range, &mapped)))
    {
        std::cout << "[Validation] readback map failed\n";
        return false;
    }

    const std::uint8_t* base = static_cast<const std::uint8_t*>(mapped);
    const std::uint32_t* results = reinterpret_cast<const std::uint32_t*>(base + m_resultsOffset);
    const float* dump = reinterpret_cast<const float*>(base + m_dumpOffset);

    // CS 用 asfloat(uintValue) 把整数写进 float4，所以这里必须做**位重解释**
    // 而不是数值转换（直接把 1.3e-43 转成 uint 会得到 0）。
    auto FloatBitsToUint = [](float value) -> std::uint32_t
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    };

    const std::uint32_t hlslSize = FloatBitsToUint(dump[0]);
    const std::uint32_t cpuExpectedSize = FloatBitsToUint(dump[1]);
    const std::uint32_t gpuInstanceCount = FloatBitsToUint(dump[2]);
    const std::uint32_t gpuDumpCount = FloatBitsToUint(dump[3]);

    bool allPassed = true;

    std::cout << "\n===== GPU Instance Data Validation (M9) =====\n";

    // ---- 检查 1：HLSL 与 C++ 的 sizeof(InstanceData) 是否一致 ----
    std::cout << "[1] Struct size\n"
              << "    C++  sizeof(InstanceData) = " << sizeof(InstanceData) << "\n"
              << "    HLSL sizeof(InstanceData) = " << hlslSize << "\n"
              << "    (HLSL 上报的 CPU 期望值 = " << cpuExpectedSize << ")\n";
    if (hlslSize == sizeof(InstanceData) && hlslSize == cpuExpectedSize)
    {
        std::cout << "    -> MATCH\n";
    }
    else
    {
        std::cout << "    -> **MISMATCH**：两侧布局不一致，从第二个实例起会整体错位\n";
        allPassed = false;
    }

    // ---- 检查 2：逐实例自洽性（按错误码 bit 分类统计）----
    const std::uint32_t checkedCount = std::min(gpuInstanceCount,
                                                static_cast<std::uint32_t>(cpuInstances.size()));
    std::uint32_t errorCount = 0;
    std::uint32_t bitCounts[5] = {};
    std::uint32_t firstErrors[8] = {};
    std::uint32_t firstErrorCount = 0;

    for (std::uint32_t i = 0; i < checkedCount; ++i)
    {
        const std::uint32_t code = results[i];
        if (code == 0u)
        {
            continue;
        }
        ++errorCount;
        for (int bit = 0; bit < 5; ++bit)
        {
            if ((code & (1u << bit)) != 0u)
            {
                ++bitCounts[bit];
            }
        }
        if (firstErrorCount < 8u)
        {
            firstErrors[firstErrorCount++] = i;
        }
    }

    static const char* const kBitNames[5] = {
        "world 平移 != boundingSphere.xyz",
        "包围球半径 <= 0",
        "padding != 0（步长错位时最敏感）",
        "mesh/material 索引异常",
        "world 非仿射（最后一列 != (0,0,0,1)）",
    };

    std::cout << "[2] Per-instance consistency\n"
              << "    checked instances = " << checkedCount << " / " << gpuInstanceCount << "\n"
              << "    failed instances  = " << errorCount << "\n";
    for (int bit = 0; bit < 5; ++bit)
    {
        if (bitCounts[bit] > 0)
        {
            std::cout << "      bit " << bit << " (" << kBitNames[bit] << "): "
                      << bitCounts[bit] << "\n";
        }
    }
    if (errorCount > 0)
    {
        std::cout << "    first failing indices:";
        for (std::uint32_t i = 0; i < firstErrorCount; ++i)
        {
            std::cout << " " << firstErrors[i];
        }
        std::cout << "\n    -> **FAILED**\n";
        allPassed = false;
    }
    else
    {
        std::cout << "    -> PASSED (all " << checkedCount << " instances consistent)\n";
    }

    // ---- 检查 3：原始字节逐字节比对 ----
    // 这是最强的等价性证明：GPU 读出来的 96 字节必须与 CPU 内存里的完全一致。
    const std::uint32_t dumpCount = std::min({gpuDumpCount, kMaxDumpInstances,
                                              static_cast<std::uint32_t>(cpuInstances.size())});
    std::uint32_t mismatched = 0;
    std::uint32_t firstMismatchIndex = 0;
    bool haveMismatch = false;

    for (std::uint32_t i = 0; i < dumpCount; ++i)
    {
        // 注意单位：dump 是 float*，而 kDumpInfoFloats / kFloatsPerInstance 都是
        // **float4 的个数**，因此要整体乘以 4 才是 float 的偏移。
        const float* gpuBytes =
            dump + (kDumpInfoFloats + i * kFloatsPerInstance) * 4u;
        if (std::memcmp(gpuBytes, &cpuInstances[i], sizeof(InstanceData)) != 0)
        {
            ++mismatched;
            if (!haveMismatch)
            {
                haveMismatch = true;
                firstMismatchIndex = i;
            }
        }
    }

    std::cout << "[3] Byte-exact comparison of the first " << dumpCount << " instances\n"
              << "    memcmp(CPU InstanceData, GPU-read InstanceData)\n";
    if (mismatched == 0)
    {
        std::cout << "    -> IDENTICAL (" << dumpCount << "/" << dumpCount << " instances)\n";
    }
    else
    {
        std::cout << "    -> **" << mismatched << " instance(s) differ**, first at index "
                  << firstMismatchIndex << "\n";

        // 定位第一个不同的字节，并按字段名报告 —— 这比「96 字节里有差异」有用得多
        const std::uint8_t* cpuBytes =
            reinterpret_cast<const std::uint8_t*>(&cpuInstances[firstMismatchIndex]);
        const std::uint8_t* gpuBytes = reinterpret_cast<const std::uint8_t*>(
            dump + (kDumpInfoFloats + firstMismatchIndex * kFloatsPerInstance) * 4u);

        int firstDiffByte = -1;
        int diffByteCount = 0;
        for (int b = 0; b < static_cast<int>(sizeof(InstanceData)); ++b)
        {
            if (cpuBytes[b] != gpuBytes[b])
            {
                if (firstDiffByte < 0)
                {
                    firstDiffByte = b;
                }
                ++diffByteCount;
            }
        }

        const char* fieldName = "unknown";
        if (firstDiffByte >= 0)
        {
            if (firstDiffByte < 64) fieldName = "world (float4x4)";
            else if (firstDiffByte < 80) fieldName = "boundingSphere (float4)";
            else if (firstDiffByte < 84) fieldName = "meshIndex (uint)";
            else if (firstDiffByte < 88) fieldName = "materialIndex (uint)";
            else fieldName = "padding (uint2)";
        }

        std::cout << "    first differing byte = " << firstDiffByte
                  << " (field: " << fieldName << "), " << diffByteCount
                  << " of " << sizeof(InstanceData) << " bytes differ\n";

        // 把该处的 CPU / GPU 浮点值都打出来，便于直接对照
        if (firstDiffByte >= 0)
        {
            const int floatIndex = firstDiffByte / 4;
            const float* cpuFloats = reinterpret_cast<const float*>(cpuBytes);
            const float* gpuFloats = reinterpret_cast<const float*>(gpuBytes);
            std::cout << "    float[" << floatIndex << "]  CPU = " << cpuFloats[floatIndex]
                      << "   GPU = " << gpuFloats[floatIndex] << "\n";
        }
        allPassed = false;
    }

    std::cout << "===== Validation " << (allPassed ? "PASSED" : "FAILED") << " =====\n\n";

    m_readbackBuffer->Unmap(0, nullptr);
    return allPassed;
}
