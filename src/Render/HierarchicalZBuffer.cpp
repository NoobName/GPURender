#include "Render/HierarchicalZBuffer.h"

#include <iostream>

#include "Render/ShaderCompiler.h"

namespace
{
constexpr UINT kHZBThreadGroupSize = 8; // 与两个 .hlsl 的 [numthreads(8,8,1)] 一致
constexpr UINT kHZBConstantDwords = 8;

// 下一级尺寸 = max(1, floor(size / 2))。
//
// **必须用 floor 而不是 ceil**：D3D12 的纹理 mip 链尺寸是由 API 固定的，
// 它按 `max(1, floor(w >> i))` 递推。如果我们自己用 ceil（`(w+1)/2`），
// 得到的级数会比 API 允许的多 —— 例如 640x360 在 API 下是 10 级
// （640→320→160→80→40→20→10→5→2→1），ceil 会算出 11 级，
// 而 `MipLevels = 11` 对 640 宽是**非法值**：它不会被 API 校验拦住，
// 而是直接在驱动内部崩溃（本阶段实际踩到的坑）。
//
// 代价是奇数尺寸下**最右/最下一列像素会被下采样漏掉**（floor 规则必然如此）。
// 这会影响保守性，所以两个 CS 用「最后一个目标像素延伸读到源末尾」的方式
// 把漏掉的像素补回来 —— 详见 HZBDownsampleCS.hlsl 里的说明。
UINT HalfSize(UINT value)
{
    return (value > 1u) ? (value / 2u) : 1u;
}

// D3D12 允许的最大 mip 级数：floor(log2(max(w, h))) + 1
UINT MaxMipLevels(UINT width, UINT height)
{
    UINT levels = 1;
    UINT largest = (width > height) ? width : height;
    while (largest > 1u)
    {
        largest /= 2u;
        ++levels;
    }
    return levels;
}

bool CreateRootSignature(ID3D12Device* device,
                         UINT numSrvDescriptors,
                         UINT numUavDescriptors,
                         ID3D12RootSignature** out)
{
    D3D12_ROOT_PARAMETER params[3] = {};

    // 参数 0：root constants（源/目标尺寸 + mip 索引）
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = kHZBConstantDwords;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    UINT paramCount = 1;
    D3D12_DESCRIPTOR_RANGE srvRange = {};

    if (numSrvDescriptors > 0)
    {
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = numSrvDescriptors;
        srvRange.BaseShaderRegister = 0; // t0
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        params[paramCount].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[paramCount].DescriptorTable.NumDescriptorRanges = 1;
        params[paramCount].DescriptorTable.pDescriptorRanges = &srvRange;
        params[paramCount].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        ++paramCount;
    }

    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = numUavDescriptors;
    uavRange.BaseShaderRegister = 0; // u0
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[paramCount].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[paramCount].DescriptorTable.NumDescriptorRanges = 1;
    params[paramCount].DescriptorTable.pDescriptorRanges = &uavRange;
    params[paramCount].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    ++paramCount;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = paramCount;
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

bool CreateComputePipeline(ID3D12Device* device,
                           ID3D12RootSignature* rootSignature,
                           const wchar_t* shaderFile,
                           ID3D12PipelineState** out)
{
    std::string errorMsg;
    std::vector<std::uint8_t> bytecode = ShaderCompiler::Compile(shaderFile, L"main",
                                                                 L"cs_6_0", errorMsg);
    if (bytecode.empty())
    {
        std::wcerr << L"[HZB] Compute shader compile failed: " << shaderFile << L"\n";
        std::cerr << errorMsg << "\n";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature;
    psoDesc.CS = { bytecode.data(), bytecode.size() };
    psoDesc.NodeMask = 0;
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    return SUCCEEDED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(out)));
}
} // namespace

bool HierarchicalZBuffer::Initialize(ID3D12Device* device,
                                     UINT sourceWidth,
                                     UINT sourceHeight,
                                     ID3D12DescriptorHeap* descriptorHeap,
                                     UINT descriptorSize,
                                     UINT firstMipUavSlot,
                                     UINT srvSlot)
{
    m_firstMipUavSlot = firstMipUavSlot;
    m_srvSlot = srvSlot;

    // ---- 1. 计算 mip 链尺寸 ----
    //
    // 第 0 级 = 全分辨率的一半（见 HZBInitCS.hlsl 顶部关于「为什么 mip 0
    // 不是全分辨率」的说明：全分辨率深度本身已经精确，不需要在金字塔里重复一份）。
    m_mipWidths[0] = HalfSize(sourceWidth);
    m_mipHeights[0] = HalfSize(sourceHeight);
    m_mipCount = 1;

    // 上限由 D3D12 的 mip 链规则决定，不能超过它
    const UINT maxLevels = MaxMipLevels(sourceWidth, sourceHeight);

    while (m_mipCount < kMaxMips &&
           (m_mipWidths[m_mipCount - 1] > 1u || m_mipHeights[m_mipCount - 1] > 1u))
    {
        m_mipWidths[m_mipCount] = HalfSize(m_mipWidths[m_mipCount - 1]);
        m_mipHeights[m_mipCount] = HalfSize(m_mipHeights[m_mipCount - 1]);
        ++m_mipCount;
    }

    if (m_mipCount > maxLevels)
    {
        // 理论上不会发生（floor 递推与 API 规则一致），保留作为断言
        std::cerr << "[HZB] internal error: mipCount " << m_mipCount
                  << " exceeds API limit " << maxLevels << "\n";
        m_mipCount = maxLevels;
    }

    if (m_mipCount >= kMaxMips)
    {
        std::cerr << "[HZB] mip chain exceeds kMaxMips (" << kMaxMips << ")\n";
        return false;
    }

    // ---- 2. 创建 HZB 纹理 ----
    //
    // 关键点：
    //   * Format = R32_FLOAT（**带类型**的浮点）——
    //     D32_FLOAT 不能建 UAV，所以 HZB 必须是独立资源；
    //   * MipLevels = m_mipCount（完整链到 1x1）；
    //   * ALLOW_UNORDERED_ACCESS（每一级都由 CS 写）。
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = m_mipWidths[0];
    texDesc.Height = m_mipHeights[0];
    // DepthOrArraySize 对 Texture2D 必须 >= 1。
    // 传 0 是非法值 —— 它不会被 API 校验拦住，而是直接在驱动内部崩溃
    // （本阶段调试时因为实验代码漏掉这一行，浪费了不少时间）。
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = static_cast<UINT16>(m_mipCount);
    texDesc.Format = DXGI_FORMAT_R32_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_texture))))
    {
        std::cerr << "[HZB] Failed to create HZB texture (" << m_mipWidths[0] << "x"
                  << m_mipHeights[0] << ", " << m_mipCount << " mips).\n";
        return false;
    }
    m_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // ---- 3. 每个 mip 一个 UAV 描述符（连续排列）----
    //
    // 连续排列是刻意的：降采样的根签名用**一张覆盖 2 个描述符的表**
    // 同时绑定「源 mip」与「目标 mip」，而描述符表只能从堆里连续取。
    // 第 i 级的表起点 = firstMipUavSlot + i，覆盖 { i, i+1 } ✓
    for (UINT mip = 0; mip < m_mipCount; ++mip)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = mip;
        uavDesc.Texture2D.PlaneSlice = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(firstMipUavSlot + mip) * descriptorSize;
        device->CreateUnorderedAccessView(m_texture.Get(), nullptr, &uavDesc, handle);
    }

    // ---- 4. SRV（可视化时读取指定的 mip）----
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = m_mipCount;
        srvDesc.Texture2D.PlaneSlice = 0;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(srvSlot) * descriptorSize;
        device->CreateShaderResourceView(m_texture.Get(), &srvDesc, handle);
    }

    // ---- 5. 两套根签名与 PSO ----
    //
    // init      ：1 张 SRV 表（深度）+ 1 张 UAV 表（HZB mip 0）
    // downsample：0 张 SRV 表 + 1 张 UAV 表（2 个连续描述符：源 mip、目标 mip）
    if (!CreateRootSignature(device, 1, 1, &m_initRootSignature) ||
        !CreateRootSignature(device, 0, 2, &m_downsampleRootSignature))
    {
        std::cerr << "[HZB] CreateRootSignature failed.\n";
        return false;
    }
    if (!CreateComputePipeline(device, m_initRootSignature.Get(),
                               L"HZBInitCS.hlsl", &m_initPipelineState) ||
        !CreateComputePipeline(device, m_downsampleRootSignature.Get(),
                               L"HZBDownsampleCS.hlsl", &m_downsamplePipelineState))
    {
        return false;
    }

    std::cout << "[HZB] Ready: " << m_mipCount << " mips, " << m_mipWidths[0] << "x"
              << m_mipHeights[0] << " -> 1x1 (Max reduction, D3D depth: near=0 far=1)\n";
    return true;
}

void HierarchicalZBuffer::Build(ID3D12GraphicsCommandList* cmd,
                                ID3D12DescriptorHeap* descriptorHeap,
                                UINT descriptorSize,
                                UINT depthSrvSlot)
{
    const D3D12_GPU_DESCRIPTOR_HANDLE heapStart =
        descriptorHeap->GetGPUDescriptorHandleForHeapStart();

    // ---- 第 0 级：从全分辨率深度做一次 2x2 Max 降采样 ----
    cmd->SetComputeRootSignature(m_initRootSignature.Get());
    cmd->SetPipelineState(m_initPipelineState.Get());

    {
        const std::uint32_t constants[kHZBConstantDwords] = {
            m_mipWidths[0] * 2u, m_mipHeights[0] * 2u, // 源尺寸（= 全分辨率深度）
            m_mipWidths[0],      m_mipHeights[0],      // 目标尺寸
            0u, 0u, 0u, 0u
        };
        cmd->SetComputeRoot32BitConstants(0, kHZBConstantDwords, constants, 0);
    }

    {
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv = heapStart;
        depthSrv.ptr += static_cast<UINT64>(depthSrvSlot) * descriptorSize;
        cmd->SetComputeRootDescriptorTable(1, depthSrv);

        D3D12_GPU_DESCRIPTOR_HANDLE dstUav = heapStart;
        dstUav.ptr += static_cast<UINT64>(m_firstMipUavSlot) * descriptorSize;
        cmd->SetComputeRootDescriptorTable(2, dstUav);
    }

    cmd->Dispatch((m_mipWidths[0] + kHZBThreadGroupSize - 1u) / kHZBThreadGroupSize,
                  (m_mipHeights[0] + kHZBThreadGroupSize - 1u) / kHZBThreadGroupSize, 1);

    // ---- 逐级降采样 ----
    for (UINT mip = 1; mip < m_mipCount; ++mip)
    {
        // 级间同步：上一级的写入必须先完成，下一级才能读它（RAW）。
        //
        // 状态没有变化（整个链常驻 UNORDERED_ACCESS），所以这一次**只能**
        // 靠 UAV barrier —— 状态转换在这里帮不上忙。
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = m_texture.Get();
            cmd->ResourceBarrier(1, &barrier);
        }

        cmd->SetComputeRootSignature(m_downsampleRootSignature.Get());
        cmd->SetPipelineState(m_downsamplePipelineState.Get());

        {
            const std::uint32_t constants[kHZBConstantDwords] = {
                m_mipWidths[mip - 1], m_mipHeights[mip - 1], // 源尺寸
                m_mipWidths[mip],     m_mipHeights[mip],     // 目标尺寸
                mip - 1, 0u, 0u, 0u
            };
            cmd->SetComputeRoot32BitConstants(0, kHZBConstantDwords, constants, 0);
        }

        // 一张表覆盖 2 个连续描述符 = { 源 mip, 目标 mip }
        {
            D3D12_GPU_DESCRIPTOR_HANDLE uavs = heapStart;
            uavs.ptr += static_cast<UINT64>(m_firstMipUavSlot + mip - 1) * descriptorSize;
            cmd->SetComputeRootDescriptorTable(1, uavs);
        }

        cmd->Dispatch((m_mipWidths[mip] + kHZBThreadGroupSize - 1u) / kHZBThreadGroupSize,
                      (m_mipHeights[mip] + kHZBThreadGroupSize - 1u) / kHZBThreadGroupSize, 1);
    }

    // 最后一次写入也要收尾，让之后任何读取（可视化 / M15 的遮挡测试）可见。
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = m_texture.Get();
        cmd->ResourceBarrier(1, &barrier);
    }
}

void HierarchicalZBuffer::TransitionTo(ID3D12GraphicsCommandList* cmd,
                                       D3D12_RESOURCE_STATES newState)
{
    if (m_state == newState || m_texture == nullptr)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = m_state;
    barrier.Transition.StateAfter = newState;
    cmd->ResourceBarrier(1, &barrier);

    m_state = newState;
}
