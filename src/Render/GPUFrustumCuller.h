#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include <DirectXMath.h>

#include "Render/VisibleInstanceList.h"

using Microsoft::WRL::ComPtr;

// GPU 视锥剔除 + Stream Compaction 的执行与验证。
//
// =============================================================================
// 每帧的 Pass 结构（M11）
// =============================================================================
//   1. ClearUnorderedAccessViewUint    把 visibleCount 清零
//   2. UAV barrier                     清零写入 vs 随后的原子操作（WAW）
//   3. Transition                      indices / count -> UNORDERED_ACCESS
//   4. UAV barrier                     与上一帧的 UAV 访问（WAW）
//   5. Dispatch FrustumCullingCS       剔除 + 原子压缩
//   6. UAV barrier                     写入 vs 之后任何读取（RAW）
//
//   **整个流程中没有一次 CPU 等待** —— 这是 M11 的硬性要求：
//   counter 的清零、可见性判定、压缩、以及可见数的产生全部在 GPU 上完成，
//   CPU 只负责把命令记录下来然后继续下一帧。
//
// =============================================================================
// 为什么需要「每帧重置计数器」
// =============================================================================
//   计数器是**跨帧累积**的：如果一个渲染帧没有先清零，
//   本帧的原子加就会从上一帧的值继续往上加，
//   输出的输出下标会从 K 开始而不是 0 —— 列表前 K 项是上一帧的陈旧数据，
//   而 visibleCount 也会变成两帧之和。
//
//   清零必须在**同一条命令列表**里、且在 dispatch 之前完成，
//   这样它天然排在本帧的渲染顺序中，不需要任何 CPU 同步。
//
// =============================================================================
// UAV Barrier（延续 M10 的规则）
// =============================================================================
//   Transition 管「这次访问是什么类型」，UAV barrier 管「两次 UAV 访问的先后」。
//   本 Pass 里有三处 UAV barrier，每一处都在注释里写明了它防止的是哪种 Hazard。
// =============================================================================
class GPUFrustumCuller
{
public:
    // 与 shaders/FrustumCullingCS.hlsl 的 [numthreads(64,1,1)] 保持一致
    static constexpr UINT kThreadGroupSize = 64;

    bool Initialize(ID3D12Device* device, std::uint32_t maxInstances);

    // 每帧调用：记录「清零计数 -> dispatch 压缩」这一整段。
    //
    // planes 必须是**已归一化**的 6 个视锥平面 (nx, ny, nz, d)，
    // 且与 CPU 侧剔除使用的完全一致 —— 这是两边结果可比的前提。
    void Record(ID3D12GraphicsCommandList* cmd,
                ID3D12DescriptorHeap* descriptorHeap,
                UINT descriptorSize,
                UINT instanceSrvSlot,
                VisibleInstanceList& visibleList,
                const DirectX::XMFLOAT4 planes[6],
                std::uint32_t instanceCount);

    // 调试用（按 G 键 / --compare-cull）：把压缩列表读回来与 CPU 结果对比。
    // **这是一条独立的、会阻塞的验证路径**，不参与正常渲染流程。
    bool ReadbackAndCompare(ID3D12Device* device,
                            ID3D12CommandQueue* queue,
                            ID3D12Fence* fence,
                            HANDLE fenceEvent,
                            UINT64& fenceValue,
                            VisibleInstanceList& visibleList,
                            const std::vector<std::uint32_t>& cpuVisibleIndices,
                            std::uint32_t instanceCount);

    // 最近一次对比的结果（供屏幕统计显示）。
    std::uint32_t GetLastCpuVisibleCount() const { return m_cpuVisibleCount; }
    std::uint32_t GetLastGpuVisibleCount() const { return m_gpuVisibleCount; }
    std::uint32_t GetLastMismatchCount() const { return m_mismatchCount; }
    bool HasComparisonResult() const { return m_hasComparison; }

    // 每帧 dispatch 的线程组数（= ceil(instanceCount / 64)），供统计显示
    static std::uint32_t GetGroupCount(std::uint32_t instanceCount)
    {
        return (instanceCount + kThreadGroupSize - 1u) / kThreadGroupSize;
    }

private:
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;

    // READBACK Heap：
    //   [0, 4)                 -> visibleCount（1 个 uint）
    //   [256, 256 + 4*maxInst) -> 压缩后的可见索引列表
    // 两段按 256 字节对齐，避免 CopyBufferRegion 的对齐问题。
    ComPtr<ID3D12Resource> m_readbackBuffer;

    std::uint32_t m_capacity = 0;
    UINT64 m_countReadbackOffset = 0;
    UINT64 m_indicesReadbackOffset = 0;

    std::uint32_t m_cpuVisibleCount = 0;
    std::uint32_t m_gpuVisibleCount = 0;
    std::uint32_t m_mismatchCount = 0;
    bool m_hasComparison = false;
};
