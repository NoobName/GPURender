#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

#include "Render/VisibleInstanceList.h"

using Microsoft::WRL::ComPtr;

// GPU-Driven 渲染所需的间接绘制命令基础设施（M12 / M16）。
//
// =============================================================================
// 它包含三样东西
// =============================================================================
//   1. **Indirect Argument Buffer**
//      DEFAULT Heap 的 ByteAddressBuffer，存放 D3D12_DRAW_INDEXED_ARGUMENTS 数组。
//      每条命令 20 字节，由 GenerateDrawCommandsCS 在 GPU 上填写。
//
//   2. **Command Signature**
//      D3D12_COMMAND_SIGNATURE_DESC，描述「参数缓冲里的字节如何解释成一条命令」。
//      ByteStride = 20（sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)），
//      ArgumentType = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED。
//      没有它，ExecuteIndirect 只是「一块内存」；有了它才成为「命令」。
//
//   3. **命令生成 Compute Pass**
//      读 LOD 元数据 + 每级的实例计数器，为**每个 LOD** 写出一条命令。
//
// =============================================================================
// M16 的关键变化：从「一条命令」变成「每个 LOD 一条」
// =============================================================================
//   M12 的做法是全世界只有一条 DrawIndexed 命令，所有可见实例共用同一份几何。
//   一旦要**按实例选不同 LOD**，这个前提就不成立了 ——
//   因为 IndexCount / StartIndex / BaseVertex 是**每条命令一份**的。
//
//   所以现在是：
//
//       第 lod 条命令（位于字节偏移 lod * 20）：
//           +0  IndexCountPerInstance = LODRange[lod].indexCount
//           +4  InstanceCount         = 该级的实例数（GPU 计数器）
//           +8  StartIndexLocation    = LODRange[lod].indexOffset
//          +12  BaseVertexLocation    = LODRange[lod].baseVertex
//          +16  StartInstanceLocation = 0（恒为 0，理由见 GenerateDrawCommandsCS.hlsl）
//
//   CPU 侧只需知道「有几级 LOD」这一个常数，不需要知道任何一级有多少实例。
//   ExecuteIndirect 的 MaxCommandCount = lodCount（4）——
//   相比 M12 的「常数 1」是一次**退让**：命令条数不再是 1，而是等于 LOD 级数。
//   但每一条的 InstanceCount 仍然完全由 GPU 决定。
//
// =============================================================================
// 资源说明
// =============================================================================
//   参数缓冲（Indirect Argument Buffer）
//     用途    ：存放 lodCount 条 DrawIndexed 命令
//     Heap    ：DEFAULT（必须，因为 CS 要写它）
//     Layout  ：第 lod 条命令位于 [lod*20, lod*20+20)
//     State   ：UNORDERED_ACCESS（CS 写） <-> INDIRECT_ARGUMENT（ExecuteIndirect 读）
//               **ExecuteIndirect 之前必须转到 INDIRECT_ARGUMENT**，
//               D3D12 明确要求（Debug Layer 会报状态不匹配）。
//     同步    ：CS 写完之后两个 UAV barrier（RAW / WAW），再做状态转换。
//
//   LOD 元数据缓冲由 Renderer 拥有并上传（见 MeshLOD.h），
//   本类只负责在命令生成时绑定它的 SRV —— 这样两边的生命周期互不纠缠。
//
//   为什么要专门的状态 D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT：
//     它告诉驱动这块内存会被**命令处理器**读取，而不是被着色器读取。
//     驱动据此决定是否需要刷新/失效相关缓存 —— 用错状态会读到陈旧的命令。
// =============================================================================
class IndirectDrawCommands
{
public:
    static constexpr UINT kDrawIndexedArgumentSize = 20; // sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)

    // lodCount：命令条数（= LOD 级数）。M12~M15 用 1，M16 起用 4。
    bool Initialize(ID3D12Device* device,
                    std::uint32_t maxInstances,
                    std::uint32_t lodCount,
                    ID3D12DescriptorHeap* descriptorHeap,
                    UINT descriptorSize,
                    UINT argumentsUavSlot);

    ID3D12Resource* GetArgumentBuffer() const { return m_arguments.Get(); }
    ID3D12CommandSignature* GetCommandSignature() const { return m_commandSignature.Get(); }
    UINT GetArgumentsUavSlot() const { return m_argumentsUavSlot; }
    std::uint32_t GetMaxInstances() const { return m_maxInstances; }
    std::uint32_t GetLODCount() const { return m_lodCount; }

    void TransitionTo(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES newState);

    // 临时诊断：读回参数缓冲的前几条命令并打印
    bool DebugReadbackFirstCommands(ID3D12Device* device, ID3D12CommandQueue* queue,
                                    ID3D12Fence* fence, HANDLE fenceEvent,
                                    UINT64& fenceValue, UINT commandCount);

    // 记录命令生成 Pass。
    //
    //   visibleList         提供**每级**的实例计数器（SRV）
    //   lodMetadataSrvSlot  LOD 元数据缓冲的 SRV 槽位
    void Record(ID3D12GraphicsCommandList* cmd,
                ID3D12DescriptorHeap* descriptorHeap,
                UINT descriptorSize,
                VisibleInstanceList& visibleList,
                UINT lodMetadataSrvSlot);

private:
    ComPtr<ID3D12Resource> m_arguments;        // DEFAULT + UAV，存放命令数组
    ComPtr<ID3D12CommandSignature> m_commandSignature;

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;

    std::uint32_t m_maxInstances = 0;
    std::uint32_t m_lodCount = 1;
    UINT m_argumentsUavSlot = 0;

    D3D12_RESOURCE_STATES m_state = D3D12_RESOURCE_STATE_COMMON;
};
