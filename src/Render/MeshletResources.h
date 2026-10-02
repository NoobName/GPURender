#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "Render/MeshletBuilder.h"

using Microsoft::WRL::ComPtr;

// =============================================================================
// M17：Meshlet 数据的 GPU 侧资源
// =============================================================================
// 把 MeshletBuilder 产出的四份 CPU 数组上传成四个 DEFAULT Heap 的
// StructuredBuffer，并各建一个 SRV。
//
// -----------------------------------------------------------------------------
// Meshlet GPU Memory Layout（要求记录的内容）
// -----------------------------------------------------------------------------
//   四个独立缓冲，而不是一个交错的大缓冲 —— 理由是**访问模式完全不同**：
//
//   ① MeshletBuffer        StructuredBuffer<MeshletGPU>        stride 16
//        每个 meshlet 一条记录。剔除/调度阶段**每个 meshlet 读一次**。
//        第 i 条 = meshlets[i]
//
//   ② MeshletVertexBuffer  StructuredBuffer<uint32_t>          stride  4
//        所有 meshlet 的「局部槽位 -> 原始顶点索引」映射，**按 meshlet 分段拼接**。
//        第 i 个 meshlet 的局部槽位 j 对应 vertexIndices[meshlets[i].vertOffset + j]。
//        为什么要这层间接：meshlet 内部三角形用 10 位**局部**索引（见 ④），
//        必须先查这张表才能拿到真正的顶点缓冲索引。
//
//   ③ MeshletPrimBuffer    StructuredBuffer<MeshletTriGPU>     stride  4
//        所有 meshlet 的三角形，同样按 meshlet 分段拼接。
//        第 i 个 meshlet 的第 k 个三角形 = primIndices[meshlets[i].primOffset + k]。
//        每个三角形是 10+10+10 位打包的局部索引，**恰好 4 字节**。
//
//   ④ MeshletBoundsBuffer  StructuredBuffer<MeshletBoundsGPU>  stride 32
//        每个 meshlet 的包围球 + 法线锥，同样按 meshlet 索引一一对应。
//        M17 用它做可视化；M18 用它做剔除（包围球做视锥剔除，法线锥做背面锥剔除）。
//
//   为什么不交错成 AoS：
//     ③ 和 ④ 的读取频率差了数量级 —— 剔除阶段只碰 ④（每 meshlet 一次），
//     真正输出几何时才碰 ③（每三角形一次）。分成独立缓冲可以让剔除阶段
//     完全不触碰三角形数据，缓存局部性远好于把它们塞进同一条记录。
//
// -----------------------------------------------------------------------------
// 资源状态与同步
// -----------------------------------------------------------------------------
//   Heap   : DEFAULT（GPU 只读 —— 但 SRV 需要 NON_PIXEL_SHADER_RESOURCE 状态）
//   State  : COPY_DEST --(barrier)--> NON_PIXEL_SHADER_RESOURCE，之后**永不再变**
//            （M17/M18 都不会写它们，所以不需要每帧转换）
//   同步   : 上传命令与主命令在同一队列上顺序执行，无需额外 fence；
//            staging 缓冲要活到上传命令执行完，因此由调用方统一收集延后释放。
// =============================================================================
class MeshletResources
{
public:
    // 本类需要的 SRV 连续槽位数（从 firstSrvSlot 开始）。
    //   相对偏移 0 -> MeshletGPU 数组
    //   相对偏移 1 -> UniqueVertexIndices
    //   相对偏移 2 -> PrimitiveIndices
    //   相对偏移 3 -> CullData（包围球 + 法线锥）
    static constexpr UINT kSrvCount             = 4;
    static constexpr UINT kMeshletSrvOffset     = 0;
    static constexpr UINT kVertexIndexSrvOffset = 1;
    static constexpr UINT kPrimIndexSrvOffset   = 2;
    static constexpr UINT kBoundsSrvOffset      = 3;

    // 从 builder 取数据并上传。失败返回 false。
    bool Initialize(ID3D12Device* device,
                    ID3D12GraphicsCommandList* cmd,
                    const MeshletBuilder& builder,
                    ID3D12DescriptorHeap* descriptorHeap,
                    UINT descriptorSize,
                    UINT firstSrvSlot,
                    std::vector<ComPtr<ID3D12Resource>>& stagingOut);

    ID3D12Resource* GetMeshletBuffer() const { return m_meshletBuffer.Get(); }
    ID3D12Resource* GetVertexIndexBuffer() const { return m_vertexIndexBuffer.Get(); }
    ID3D12Resource* GetPrimIndexBuffer() const { return m_primIndexBuffer.Get(); }
    ID3D12Resource* GetBoundsBuffer() const { return m_boundsBuffer.Get(); }

    // GPU 侧占比统计（用于文档与日志）
    std::uint64_t GetTotalGpuBytes() const { return m_totalGpuBytes; }

private:
    ComPtr<ID3D12Resource> m_meshletBuffer;
    ComPtr<ID3D12Resource> m_vertexIndexBuffer;
    ComPtr<ID3D12Resource> m_primIndexBuffer;
    ComPtr<ID3D12Resource> m_boundsBuffer;

    std::uint64_t m_totalGpuBytes = 0;
};
