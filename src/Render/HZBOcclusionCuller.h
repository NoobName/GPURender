#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <DirectXMath.h>

#include <cstdint>

#include "Render/VisibleInstanceList.h"

using Microsoft::WRL::ComPtr;

// HZB 遮挡剔除（M15）。
//
// =============================================================================
// 它在整个可见性流水线里的位置
// =============================================================================
//   实例 (N)                                   可见性来源
//     │ FrustumCullingCS                         6 平面测试
//     ▼
//   候选列表 + 候选数          ← 视锥剔除的结果（M11 压缩）
//     │                                          （Depth Prepass 画的就是它）
//     ▼
//   HZBOcclusionCS（本文件）                    HZB 保守深度比对
//     │
//     ▼
//   可见列表 + 可见数          ← 最终可见（Main Pass 只画这些）
//
//   Depth Prepass 与 Main Pass 用的是**不同的索引列表**：
//   前者是"视锥内全部"，后者是"视锥内且未被遮挡"。
//   因此需要两次命令生成 —— 第二次写出的命令会覆盖第一次的，
//   这是安全的，因为命令缓冲是**顺序消费**的（Depth Pass 已经执行完）。
//
// =============================================================================
// 保守性是第一原则
// =============================================================================
//   遮挡剔除绝不能漏画。判定「被遮挡」必须是**充分条件**。
//   CS 里所有不确定的情形都偏向 Visible，本类额外提供：
//     * enabled 开关（A/B 对比用）
//     * depthBias 参数（吸收浮点误差，避免深度相等时误剔除）
//     * conservativePassCount 统计（衡量"无法判定"的次数）
//
// =============================================================================
// 资源说明
// =============================================================================
//   m_visible : 遮挡剔除的最终可见列表（结构复用 M11 的 VisibleInstanceList）
//
//   m_stats : RWByteAddressBuffer，16 字节，DEFAULT Heap + UAV
//             偏移  0 : 候选数
//             偏移  4 : 因遮挡被剔除数
//             偏移  8 : 保守放行数（球在相机后 / 穿近平面 / 完全出屏）
//             偏移 12 : 累计采样的 HZB 像素数（观察 mip 选择是否合理）
//
//   m_statsZero : UPLOAD Heap 上的一块 16 字节全零缓冲。
//             **每帧必须清零统计** —— CS 里用的是 InterlockedAdd（累加语义），
//             不清零的话数字会一直涨。用 CopyBufferRegion 从这块零缓冲拷过去
//             比 ClearUnorderedAccessViewUint 更简单：后者需要一对
//             「shader-visible GPU 句柄 + non-shader-visible CPU 句柄」，
//             而这里只需要一个源指针。
//
//   m_statsReadback : READBACK Heap 上的环形缓冲（kFrameRingSize 帧）。
//             采用**延迟读回**，与 M13 的 GPUProfiler 同样的理由：
//             绝不允许为了拿统计数字而每帧同步等待 GPU。
//
//   m_occlusionSrvSlots : occlusion CS 需要「实例表 / 候选列表 / HZB」
//             三张 SRV **在堆里连续**（描述符表只能取连续区间），
//             而这三者在全局规划里相距很远。所以本类在固定槽位
//             （statsSrvSlot + 1 起）**每帧重新创建**这三个视图。
//             CreateShaderResourceView 是廉价操作，换来的是完全解耦的槽位规划。
// =============================================================================
class HZBOcclusionCuller
{
public:
    static constexpr UINT kStatsDwords = 4;
    static constexpr UINT kFrameRingSize = 3;
    // occlusion CS 自己的 SRV 表：实例表 / 候选列表 / HZB
    static constexpr UINT kOcclusionSrvCount = 3;

    // 统计缓冲里的 DWORD 偏移
    enum StatsOffset : UINT
    {
        kStatCandidateCount = 0,
        kStatOccludedCount = 1,
        kStatConservativePassCount = 2,
        kStatSampledTexels = 3,
    };

    struct FrameStats
    {
        std::uint32_t candidateCount = 0;
        std::uint32_t occludedCount = 0;
        std::uint32_t conservativePassCount = 0;
        std::uint32_t sampledTexels = 0;
        bool valid = false;
    };

    bool Initialize(ID3D12Device* device,
                    std::uint32_t maxInstances,
                    ID3D12DescriptorHeap* descriptorHeap,
                    UINT descriptorSize,
                    UINT statsUavSlot,
                    UINT statsSrvSlot);

    // 记录一次遮挡剔除。
    //
    //   cmd                当前帧的命令列表
    //   candidates         M11 压缩出的候选列表（视锥内），作为输入
    //   instanceBuffer     实例表（StructuredBuffer<InstanceData>）
    //   hzbResource        HZB 纹理（作为 SRV 读第 0 级 + 指定 mip）
    //   viewProj           行主序 view*proj
    //   proj00 / proj11    投影矩阵的 [0][0] / [1][1]
    void Record(ID3D12GraphicsCommandList* cmd,
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
                float depthBias);

    // 记录「把统计拷进 readback 环形缓冲」（必须在帧内、Close 之前调用）。
    void ResolveStats(ID3D12GraphicsCommandList* cmd, UINT frameIndex);

    // 读取**已经完成**的某一帧的统计（不会阻塞）。
    FrameStats ReadbackStats(UINT laggedFrameIndex) const;

    VisibleInstanceList& GetVisibleList() { return m_visible; }
    const VisibleInstanceList& GetVisibleList() const { return m_visible; }
    UINT GetOcclusionSrvSlot() const { return m_statsSrvSlot + 1; }

    bool IsInitialized() const { return m_initialized; }

private:
    VisibleInstanceList m_visible;

    ComPtr<ID3D12Resource> m_stats;
    ComPtr<ID3D12Resource> m_statsZero;     // UPLOAD：每帧清零用
    ComPtr<ID3D12Resource> m_statsReadback; // READBACK：环形

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;

    UINT m_statsUavSlot = 0;
    UINT m_statsSrvSlot = 0;
    UINT m_maxInstances = 0;
    bool m_initialized = false;
};
