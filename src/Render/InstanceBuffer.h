#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "Scene/InstanceData.h"

using Microsoft::WRL::ComPtr;

// GPU 常驻的实例数据缓冲。
//
// =============================================================================
// 资源说明
// =============================================================================
//   用途    ：以 StructuredBuffer<InstanceData> 的形式，把整个场景的实例元数据
//             （变换 / 包围球 / 网格索引 / 材质索引）放在 GPU 上，
//             供后续里程碑的 Compute Shader 剔除与间接绘制读取。
//   Heap    ：**DEFAULT Heap（显存）**。这是 M9 的核心要求 ——
//             M7/M8 每帧要往 Upload Heap 写 24 MB 常量，那是 CPU-Driven 的
//             固有开销；把实例数据一次性放进显存后，每帧不再需要上传。
//   Layout  ：元素 = InstanceData（96 字节，见 src/Scene/InstanceData.h 的布局表），
//             紧凑排列，第 i 个实例位于 i * 96 字节。
//             元素步长必须与 HLSL 的 sizeof(InstanceData) 一致，
//             否则从第二个实例起全部错位。
//   状态    ：上传后为 NON_PIXEL_SHADER_RESOURCE
//             （M9 只给 Compute Shader 读；主渲染路径仍是 CPU-Driven）。
//   生命周期：与 Renderer 同寿命，按**最大实例数**预分配，
//             切换场景规模时只重新上传数据、不重建资源。
//   同步    ：上传走 Upload Heap staging + CopyBufferRegion，
//             由调用方在同一次提交里等待 fence 完成后再释放 staging。
// =============================================================================
class InstanceBuffer
{
public:
    // 按 maxInstances 预分配（DEFAULT Heap），并写入初始数据。
    // srvIndex 是 SRV 描述符堆里的槽位号。
    bool Initialize(ID3D12Device* device,
                    ID3D12GraphicsCommandList* cmd,
                    std::uint32_t maxInstances,
                    const std::vector<InstanceData>& instances,
                    ID3D12DescriptorHeap* srvHeap,
                    UINT srvDescriptorSize,
                    UINT srvIndex,
                    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& stagingOut);

    // 重新上传实例数据（切换场景规模时调用）。
    // 传入的实例数不能超过 Initialize 时的 maxInstances。
    // 资源不重建，只更新内容 —— 因此 SRV 也不需要重建。
    bool Upload(ID3D12Device* device,
                ID3D12GraphicsCommandList* cmd,
                const std::vector<InstanceData>& instances,
                std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& stagingOut);

    ID3D12Resource* GetResource() const { return m_resource.Get(); }
    std::uint32_t GetInstanceCount() const { return m_instanceCount; }
    std::uint32_t GetCapacity() const { return m_capacity; }
    UINT GetSrvIndex() const { return m_srvIndex; }

    // 元素步长（字节）。就是 sizeof(InstanceData)。
    static constexpr UINT GetStride() { return static_cast<UINT>(sizeof(InstanceData)); }

private:
    // 把 instances 写进 staging 并记录 copy + 状态转换。
    bool UploadInternal(ID3D12Device* device,
                        ID3D12GraphicsCommandList* cmd,
                        const std::vector<InstanceData>& instances,
                        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& stagingOut);

    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    std::uint32_t m_capacity = 0;
    std::uint32_t m_instanceCount = 0;
    UINT m_srvIndex = 0;
    D3D12_RESOURCE_STATES m_state = D3D12_RESOURCE_STATE_COMMON;
};
