#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

using Microsoft::WRL::ComPtr;

// 层级深度金字塔（Hierarchical Z-Buffer / Depth Pyramid）。
//
// =============================================================================
// 它是什么
// =============================================================================
//   把 Depth Prepass 的全分辨率深度，逐级做 2x2 的 **Max 归约**，
//   直到 1x1，形成一条完整的 mip 链：
//
//     depth (1280x720, 精确深度)
//        │  2x2 Max（HZBInitCS）
//        ▼
//     HZB mip 0  (640x360)
//        │  2x2 Max（HZBDownsampleCS）
//        ▼
//     HZB mip 1  (320x180)
//        ▼  ...  ▼
//     HZB mip N-1 (1x1)   ← 等于**全屏最深**的深度
//
//   不变量：**第 i 级的每个像素 = 它覆盖的屏幕区域内所有第 0 级深度的最大值**。
//
// =============================================================================
// Reduction Convention（必须显式记录，因为它决定了整个金字塔的正确性）
// =============================================================================
//   * 投影约定：**传统 DirectX 深度** —— NDC z ∈ [0,1]，
//                  近平面 -> 0，远平面 -> 1，**深度值越大表示越远**。
//   * 归约算子：**Max**（取块内最大深度 = 最远的深度）。
//
//   为什么是 Max（保守遮挡剔除的充分条件）：
//     设块内深度集合 D，d_max = max(D)，待测物体最近深度 z_obj。
//       若 z_obj > d_max，则 z_obj > 所有 d ∈ D
//       → 物体比该块内每一个像素都远 → 确定被完全遮挡 ✓
//     若改用 Min，条件 z_obj > d_min 无法排除「存在更近的像素」，
//       会把实际可见的物体判为遮挡 → **误剔除** ✗
//
//   > **换 Reverse-Z（近 1 / 远 0）时，这里必须整体改成 Min。**
//   > 代码里用 static_assert 风格的地方不多，所以这条约定写在类注释与
//   > 两个 .hlsl 的顶部 —— 改投影矩阵的人一定会看到。
//
// =============================================================================
// 资源说明
// =============================================================================
//   HZB 纹理
//     用途    ：每级 = 其覆盖区域内深度的 Max，供保守遮挡测试使用（M15）
//     Heap    ：DEFAULT
//     Format  ：DXGI_FORMAT_R32_FLOAT ——
//               必须是**带类型**的浮点格式，因为要作为 typed UAV 写入；
//               D32_FLOAT 无法创建 UAV，所以 HZB 必须是独立于深度缓冲的资源。
//     MipLevels：完整链，最后一级为 1x1
//     Flags   ：ALLOW_UNORDERED_ACCESS（每级都由 CS 写）
//     State   ：构建期间常驻 UNORDERED_ACCESS；
//               可视化时才临时切到 PIXEL_SHADER_RESOURCE。
//
//   为什么用 UAV 读源、UAV 写目标（而不是 SRV 读）：
//     每一级的输出就是下一级的输入。若每级之间都做
//     UNORDERED_ACCESS <-> NON_PIXEL_SHADER_RESOURCE 转换会产生大量 barrier；
//     而 D3D12 不允许同一资源同时处于「SRV 可读」与「UAV 可写」两个状态。
//     RWTexture2D<float>::Load 是合法读取，于是整条链只需要级间一个 UAV barrier。
//
// =============================================================================
// 同步
// =============================================================================
//   每一级 dispatch 之后插一个 **UAV barrier**：
//   它保证「该 mip 的写入」对「下一级 dispatch 的读取」可见（RAW）。
//   状态不变，所以状态转换在这里是**无关**的 —— 这是 M10/M11 反复强调过的
//   「UAV barrier 管顺序、Transition 管访问类型」。
// =============================================================================
class HierarchicalZBuffer
{
public:
    // 1280x720 从 (640,360) 到 (1,1) 需要 11 级；留出余量给更大的分辨率。
    static constexpr UINT kMaxMips = 16;
    // 每个 mip 一个 UAV 描述符槽位
    static constexpr UINT kUavSlotsPerMip = 1;

    bool Initialize(ID3D12Device* device,
                    UINT sourceWidth,
                    UINT sourceHeight,
                    ID3D12DescriptorHeap* descriptorHeap,
                    UINT descriptorSize,
                    UINT firstMipUavSlot,
                    UINT srvSlot);

    // 记录整条 mip 链的构建（mip 0 由深度生成，其余逐级降采样）。
    // depthSrvSlot 指向全分辨率深度的 SRV。
    void Build(ID3D12GraphicsCommandList* cmd,
               ID3D12DescriptorHeap* descriptorHeap,
               UINT descriptorSize,
               UINT depthSrvSlot);

    void TransitionTo(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES newState);

    UINT GetMipCount() const { return m_mipCount; }
    UINT GetMipWidth(UINT mip) const { return m_mipWidths[mip]; }
    UINT GetMipHeight(UINT mip) const { return m_mipHeights[mip]; }
    UINT GetSrvSlot() const { return m_srvSlot; }
    ID3D12Resource* GetResource() const { return m_texture.Get(); }

    // 每个 mip 的 UAV 槽位（连续排列，第 i 级 = firstMipUavSlot + i）
    UINT GetMipUavSlot(UINT mip) const { return m_firstMipUavSlot + mip; }

    // 全屏最远的深度值 = 最后一级 1x1 的值（调试/统计用）
    D3D12_RESOURCE_STATES GetState() const { return m_state; }

private:
    ComPtr<ID3D12Resource> m_texture;
    ComPtr<ID3D12RootSignature> m_initRootSignature;
    ComPtr<ID3D12RootSignature> m_downsampleRootSignature;
    ComPtr<ID3D12PipelineState> m_initPipelineState;
    ComPtr<ID3D12PipelineState> m_downsamplePipelineState;

    UINT m_mipCount = 0;
    UINT m_mipWidths[kMaxMips] = {};
    UINT m_mipHeights[kMaxMips] = {};
    UINT m_firstMipUavSlot = 0;
    UINT m_srvSlot = 0;

    D3D12_RESOURCE_STATES m_state = D3D12_RESOURCE_STATE_COMMON;
};
