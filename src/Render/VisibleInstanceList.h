#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

using Microsoft::WRL::ComPtr;

// 压缩后的可见实例列表（Stream Compaction 的输出）。
//
// =============================================================================
// 它替代了 M10 的「逐实例布尔数组」
// =============================================================================
//   M10：visibilityFlags[N]            每个实例一个 uint（0/1），100k 实例 = 400 KB
//   M11：visibleInstanceIndices[K]     只有 K 个 uint（K = 可见数）
//        + visibleCount（4 字节）
//
//   内存与下游读取量都正比于**可见数**；下游 Pass 可以顺序遍历，
//   不需要再跳过被剔除的槽位。代价是输出顺序不确定（取决于线程完成顺序），
//   以及每次写入需要一次原子操作。
//
// =============================================================================
// 资源说明
// =============================================================================
//   m_indices : RWStructuredBuffer<uint>，容量 = maxInstances（最坏情况全可见）
//               DEFAULT Heap + UAV
//               第 i 个可见实例的 ID 位于 [i*4, i*4+4)
//               有效长度由 m_count 给出，**不是**容量
//
//   m_count   : RWByteAddressBuffer，4 字节
//               DEFAULT Heap + UAV
//               偏移 0 处是一个 uint，含义 = 本帧已写入的可见实例数
//               用 ByteAddressBuffer 而非 StructuredBuffer<uint> 的原因：
//               它是一个「裸」缓冲，将来 ExecuteIndirect 的计数参数
//               可以直接指向同一块内存，不需要额外拷贝
//
//   m_clearHeap : **non-shader-visible** 的 CBV_SRV_UAV 堆（1 个描述符）
//               只给 ClearUnorderedAccessViewUint 用 —— 这个 API 需要一个
//               CPU 侧描述符句柄来指明「清哪个 UAV」，而该句柄必须来自
//               非着色器可见堆（着色器可见堆的句柄不能用于清空操作）。
//
//   状态管理：两个缓冲各自跟踪自己的状态，由 Transition*To 显式转换。
//             创建时都是 COMMON —— D3D12 会忽略 Buffer 的 InitialState
//             （M10 被 Debug Layer 的 ID=1328 警告抓过）。
//
//   每帧同步：见 GPUFrustumCuller::Record 的 barrier 序列。
// =============================================================================
class VisibleInstanceList
{
public:
    bool Initialize(ID3D12Device* device,
                    std::uint32_t maxInstances,
                    ID3D12DescriptorHeap* descriptorHeap,
                    UINT descriptorSize,
                    UINT indicesUavSlot,
                    UINT countUavSlot,
                    UINT indicesSrvSlot,
                    UINT countSrvSlot);

    ID3D12Resource* GetIndexBuffer() const { return m_indices.Get(); }
    ID3D12Resource* GetCountBuffer() const { return m_count.Get(); }

    UINT GetIndicesUavSlot() const { return m_indicesUavSlot; }
    UINT GetCountUavSlot() const { return m_countUavSlot; }

    // SRV 槽位：给 M12 的命令生成 CS 用（只读访问走只读缓存路径，
    // 比用 UAV 读更合适）。同一个资源同时拥有 UAV 与 SRV 视图是允许的。
    UINT GetIndicesSrvSlot() const { return m_indicesSrvSlot; }
    UINT GetCountSrvSlot() const { return m_countSrvSlot; }

    std::uint32_t GetCapacity() const { return m_capacity; }

    // 元素步长（字节）
    static constexpr UINT GetIndexStride() { return sizeof(std::uint32_t); }
    static constexpr UINT GetCountStride() { return sizeof(std::uint32_t); }

    // ClearUnorderedAccessViewUint 需要「资源指针 + non-shader-visible 的
    // CPU 描述符句柄」，两者都由这里提供。
    ID3D12Resource* GetCountResourceForClear() const { return m_count.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetCountClearCpuHandle() const;

    void TransitionIndicesTo(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES newState);
    void TransitionCountTo(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES newState);

    // 把 indices 缓冲里 [0, count) 的内容拷到 readback（调试用）。
    // 由 GPUFrustumCuller 直接操作资源，这里只暴露状态跟踪。

private:
    ComPtr<ID3D12Resource> m_indices;
    ComPtr<ID3D12Resource> m_count;
    ComPtr<ID3D12DescriptorHeap> m_clearHeap;

    std::uint32_t m_capacity = 0;
    UINT m_indicesUavSlot = 0;
    UINT m_countUavSlot = 0;
    UINT m_indicesSrvSlot = 0;
    UINT m_countSrvSlot = 0;

    D3D12_RESOURCE_STATES m_indicesState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES m_countState = D3D12_RESOURCE_STATE_COMMON;
};
