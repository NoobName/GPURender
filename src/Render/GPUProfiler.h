#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

using Microsoft::WRL::ComPtr;

// GPU 时间戳测量（M13）。
//
// =============================================================================
// 为什么需要它
// =============================================================================
//   M8~M12 一直在测**CPU 时间**（cull / update / record / present）。
//   但 Depth Prepass 是纯粹的 GPU 侧工作 —— 它把 CPU 的提交量**变多**
//   （多一遍 draw），却把 GPU 的像素着色量**变少**（Early-Z 提前丢弃）。
//
//   只靠 CPU 计时根本看不出这笔交易划不划算，必须直接测 GPU 时间。
//
// =============================================================================
// 原理
// =============================================================================
//   GPU 有一个自由运行的 64 位时间戳计数器。`EndQuery(TIMESTAMP)` 把
//   **命令执行到那一点时**的计数值记录下来 —— 注意它不是 CPU 发起时刻，
//   而是 GPU 流水线真正走到那条命令的时刻，这正是我们想要的。
//
//       t0  EndQuery  帧开始
//       t1  EndQuery  Depth Prepass 结束
//       t2  EndQuery  Main Pass 结束
//
//       depthMs = (t1 - t0) / frequency * 1000
//       mainMs  = (t2 - t1) / frequency * 1000
//
//   `frequency` 由 `ID3D12CommandQueue::GetTimestampFrequency` 查询，
//   单位是 tick/秒。
//
// =============================================================================
// 为什么延迟若干帧才读回
// =============================================================================
//   如果在提交后立刻 `WaitForSingleObject` 等 GPU 完成，就等于每帧强制
//   一次 CPU-GPU 同步 —— GPU 的并行度被彻底摧毁，测出来的时间也失去意义
//   （CPU 等待的时间会挤进测量区间）。
//
//   所以这里采用**环形缓冲 + 延迟读回**：
//     * kFrameCount 份查询槽位与 readback 区域，每帧用一份；
//     * 提交第 N 帧时，顺手读回第 N-kFrameCount 帧的结果（那时它早已完成，
//       Map 不会阻塞）。
//   代价是统计值滞后 kFrameCount 帧 —— 对滚动平均显示完全无所谓。
// =============================================================================
class GPUProfiler
{
public:
    // 每帧记录的时间点：
    //   0 = 帧开始（任何计算 Pass 之前）
    //   1 = GPU 剔除 + 命令生成结束
    //   2 = Depth Pass 结束
    //   3 = Main Pass 结束
    //
    // 分成四个点而不是两个，是因为「Depth Prepass 的 GPU 成本」只有在
    // 把前面的剔除/命令生成单独切出去之后才测得准 —— 否则那两个
    // dispatch 的时间会被算进深度 pass，读数会虚高好几倍。
    static constexpr UINT kTimestampsPerFrame = 4;
    static constexpr UINT kSlotFrameBegin = 0;
    static constexpr UINT kSlotCullEnd = 1;
    static constexpr UINT kSlotDepthEnd = 2;
    static constexpr UINT kSlotMainEnd = 3;

    // 环形缓冲的深度。与 Renderer 的 kFrameCount 保持一致 ——
    // 它决定了「延迟多少帧读回」，因为每个 frame slot 要等 GPU 用完才能复用。
    static constexpr UINT kFrameRingSize = 3;

    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue);

    // 记录一个时间点。slot 取 0..kTimestampsPerFrame-1。
    void WriteTimestamp(ID3D12GraphicsCommandList* cmd, UINT frameIndex, UINT slot);

    // 在命令列表关闭前调用：把本帧的时间戳解析到 readback 缓冲。
    void Resolve(ID3D12GraphicsCommandList* cmd, UINT frameIndex);

    // 读取**已经完成**的那一帧的结果（不会阻塞）。
    // 返回 true 表示拿到了有效数据。
    bool ReadbackCompletedFrame(UINT laggedFrameIndex,
                                double& outCullMs,
                                double& outDepthPassMs,
                                double& outMainPassMs);

    double GetFrequency() const { return static_cast<double>(m_frequency); }

private:
    ComPtr<ID3D12QueryHeap> m_queryHeap;
    ComPtr<ID3D12Resource> m_readbackBuffer;
    UINT64 m_frequency = 0;
    bool m_initialized = false;
};
