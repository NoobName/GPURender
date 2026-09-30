#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

#include "Render/VisibleInstanceList.h"

using Microsoft::WRL::ComPtr;

// GPU-Driven 渲染所需的间接绘制命令基础设施（M12）。
//
// =============================================================================
// 它包含三样东西
// =============================================================================
//   1. **Indirect Argument Buffer**
//      DEFAULT Heap 的 ByteAddressBuffer，存放 D3D12_DRAW_INDEXED_ARGUMENTS 数组。
//      每个可见实例一条命令，共 20 字节。
//      由 GenerateDrawCommandsCS 在 GPU 上填写。
//
//   2. **Command Signature**
//      D3D12_COMMAND_SIGNATURE_DESC，描述「参数缓冲里的字节如何解释成一条命令」。
//      ByteStride = 20（sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)），
//      ArgumentType = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED。
//      没有它，ExecuteIndirect 只是「一块内存」；有了它才成为「命令」。
//
//   3. **命令生成 Compute Pass**
//      读可见索引列表 + 计数器，写出上面的参数数组。
//
// =============================================================================
// 资源说明
// =============================================================================
//   参数缓冲（Indirect Argument Buffer）
//     用途    ：存放 K 条 DrawIndexed 命令（K = 可见实例数，由 GPU 决定）
//     Heap    ：DEFAULT（必须，因为 CS 要写它）
//     Layout  ：第 i 条命令位于 [i*20, i*20+20)
//                  +0  IndexCountPerInstance
//                  +4  InstanceCount        （恒为 1）
//                  +8  StartIndexLocation
//                 +12  BaseVertexLocation
//                 +16  StartInstanceLocation（= 真正的实例 ID）
//     State   ：UNORDERED_ACCESS（CS 写） <-> INDIRECT_ARGUMENT（ExecuteIndirect 读）
//               **ExecuteIndirect 之前必须转到 INDIRECT_ARGUMENT**，
//               这是 D3D12 明确要求的（Debug Layer 会报状态不匹配）。
//     同步    ：CS 写完之后一个 UAV barrier（RAW），再做状态转换。
//
//   为什么要专门的状态 D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT：
//     它告诉驱动这块内存会被**命令处理器**读取，而不是被着色器读取。
//     驱动据此决定是否需要刷新/失效相关缓存 —— 用错状态会读到陈旧的命令。
// =============================================================================
class IndirectDrawCommands
{
public:
    static constexpr UINT kDrawIndexedArgumentSize = 20; // sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)

    bool Initialize(ID3D12Device* device,
                    std::uint32_t maxInstances,
                    ID3D12DescriptorHeap* descriptorHeap,
                    UINT descriptorSize,
                    UINT argumentsUavSlot,
                    UINT visibleIndicesSrvSlot,
                    UINT visibleCountSrvSlot);

    ID3D12Resource* GetArgumentBuffer() const { return m_arguments.Get(); }
    ID3D12CommandSignature* GetCommandSignature() const { return m_commandSignature.Get(); }
    UINT GetArgumentsUavSlot() const { return m_argumentsUavSlot; }
    std::uint32_t GetCapacity() const { return m_capacity; }

    void TransitionTo(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES newState);

    // 临时诊断：读回参数缓冲的前几条命令并打印（验证后移除）
    bool DebugReadbackFirstCommands(ID3D12Device* device, ID3D12CommandQueue* queue,
                                    ID3D12Fence* fence, HANDLE fenceEvent,
                                    UINT64& fenceValue, UINT commandCount);

    // 记录命令生成 Pass（读可见列表与计数器，写参数缓冲）。
    void Record(ID3D12GraphicsCommandList* cmd,
                ID3D12DescriptorHeap* descriptorHeap,
                UINT descriptorSize,
                VisibleInstanceList& visibleList,
                UINT indexCount);

private:
    ComPtr<ID3D12Resource> m_arguments;        // DEFAULT + UAV，存放命令数组
    ComPtr<ID3D12CommandSignature> m_commandSignature;

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;

    std::uint32_t m_capacity = 0;
    UINT m_argumentsUavSlot = 0;
    UINT m_visibleIndicesSrvSlot = 0;
    UINT m_visibleCountSrvSlot = 0;

    D3D12_RESOURCE_STATES m_state = D3D12_RESOURCE_STATE_COMMON;
};
