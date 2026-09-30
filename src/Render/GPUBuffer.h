#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

using Microsoft::WRL::ComPtr;

// 一个 GPU Buffer 的最小抽象。
//
// 记录三样东西：
//   - ID3D12Resource（底层资源）
//   - Buffer 大小（字节）
//   - 当前资源状态（Current Resource State）
//
// 这是「轻量封装」，不是完整的 Resource Manager（M4 刻意不引入 D3D12 Memory
// Allocator，也不做 sub-allocation / 复用）。目的：把「创建 buffer + 跟踪其状态」
// 这件事从 Renderer 的渲染逻辑里抽出来，让资源生命周期清晰可见。
class GPUBuffer
{
public:
    // 创建指定 Heap Type 的 buffer。
    //   heapType     : D3D12_HEAP_TYPE_DEFAULT / UPLOAD / READBACK
    //   initialState : 期望的初始状态。注意：对普通 Buffer，运行时会忽略它
    //                  （Buffer 总是以 COMMON 创建）；但 Upload Heap 的资源
    //                  惯例上用 GENERIC_READ（作为 GPU 只读 / copy source）。
    bool Initialize(ID3D12Device* device, std::uint64_t sizeBytes,
                    D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES initialState);

    ID3D12Resource* GetResource() const { return m_resource.Get(); }
    std::uint64_t GetSizeBytes() const { return m_sizeBytes; }
    D3D12_RESOURCE_STATES GetState() const { return m_state; }
    void SetState(D3D12_RESOURCE_STATES state) { m_state = state; }
    D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const { return m_resource->GetGPUVirtualAddress(); }

    // CPU 访问（仅 Upload / Readback Heap 的 buffer 有效）。
    // readRange 为 {0,0} 表示 CPU 只写不读（避免不必要的 GPU cache flush）。
    void* Map(UINT subresource = 0, D3D12_RANGE* readRange = nullptr);
    void Unmap(UINT subresource = 0, const D3D12_RANGE* writtenRange = nullptr);

private:
    ComPtr<ID3D12Resource> m_resource;
    std::uint64_t m_sizeBytes = 0;
    D3D12_RESOURCE_STATES m_state = D3D12_RESOURCE_STATE_COMMON;
    D3D12_HEAP_TYPE m_heapType = D3D12_HEAP_TYPE_DEFAULT;
};
