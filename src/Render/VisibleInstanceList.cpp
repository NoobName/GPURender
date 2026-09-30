#include "Render/VisibleInstanceList.h"

#include <iostream>

namespace
{
// 创建一个 DEFAULT Heap 的 UAV 缓冲。
// sizeBytes 已经是最终字节数。
bool CreateUavBuffer(ID3D12Device* device, UINT64 sizeBytes, ID3D12Resource** out)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT; // UAV 不能放在 UPLOAD Heap

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // InitialState 写 COMMON：D3D12 会忽略 Buffer 的 InitialState，
    // 传别的值只会换来 Debug Layer 的 ID=1328 警告（M10 踩过）。
    return SUCCEEDED(device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(out)));
}

void TransitionResource(ID3D12GraphicsCommandList* cmd,
                        ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES& tracked,
                        D3D12_RESOURCE_STATES newState)
{
    if (resource == nullptr || tracked == newState)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = tracked;
    barrier.Transition.StateAfter = newState;
    cmd->ResourceBarrier(1, &barrier);

    tracked = newState;
}
} // namespace

bool VisibleInstanceList::Initialize(ID3D12Device* device,
                                     std::uint32_t maxInstances,
                                     ID3D12DescriptorHeap* descriptorHeap,
                                     UINT descriptorSize,
                                     UINT indicesUavSlot,
                                     UINT countUavSlot,
                                     UINT indicesSrvSlot,
                                     UINT countSrvSlot)
{
    if (maxInstances == 0)
    {
        std::cerr << "[VisibleInstanceList] maxInstances must be > 0\n";
        return false;
    }

    m_capacity = maxInstances;
    m_indicesUavSlot = indicesUavSlot;
    m_countUavSlot = countUavSlot;
    m_indicesSrvSlot = indicesSrvSlot;
    m_countSrvSlot = countSrvSlot;

    // ---- 1. 索引缓冲：容量按最大实例数（最坏情况全部可见）----
    const UINT64 indexBytes = static_cast<UINT64>(maxInstances) * sizeof(std::uint32_t);
    if (!CreateUavBuffer(device, indexBytes, &m_indices))
    {
        std::cerr << "[VisibleInstanceList] Failed to create index buffer.\n";
        return false;
    }

    // ---- 2. 计数器：4 字节 ----
    if (!CreateUavBuffer(device, sizeof(std::uint32_t), &m_count))
    {
        std::cerr << "[VisibleInstanceList] Failed to create count buffer.\n";
        return false;
    }

    // ---- 3. 在 shader-visible 堆里建两个 UAV 描述符 ----
    D3D12_CPU_DESCRIPTOR_HANDLE indexHandle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    indexHandle.ptr += static_cast<SIZE_T>(indicesUavSlot) * descriptorSize;

    D3D12_CPU_DESCRIPTOR_HANDLE countHandle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    countHandle.ptr += static_cast<SIZE_T>(countUavSlot) * descriptorSize;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN; // StructuredBuffer 必须用 UNKNOWN
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = maxInstances;
    uavDesc.Buffer.StructureByteStride = sizeof(std::uint32_t);
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    device->CreateUnorderedAccessView(m_indices.Get(), nullptr, &uavDesc, indexHandle);

    // counter 是「裸」缓冲：用 RAW（R32_UINT）视图而不是 StructuredBuffer 视图。
    // NumElements = 1，元素就是那个 uint。
    uavDesc.Format = DXGI_FORMAT_R32_UINT;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = 1;
    uavDesc.Buffer.StructureByteStride = 0; // RAW 视图必须为 0
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    device->CreateUnorderedAccessView(m_count.Get(), nullptr, &uavDesc, countHandle);

    // ---- 4. non-shader-visible 堆：只给 ClearUnorderedAccessViewUint 用 ----
    //
    // 该 API 需要一个 CPU 侧描述符指明「清哪个 UAV」，而这个句柄必须来自
    // 非着色器可见堆 —— 着色器可见堆的句柄不能用于清空操作。
    D3D12_DESCRIPTOR_HEAP_DESC clearHeapDesc = {};
    clearHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    clearHeapDesc.NumDescriptors = 1;
    clearHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // 关键：非着色器可见
    clearHeapDesc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(&clearHeapDesc, IID_PPV_ARGS(&m_clearHeap))))
    {
        std::cerr << "[VisibleInstanceList] Failed to create clear descriptor heap.\n";
        return false;
    }

    // 这个堆里的描述符内容与 shader-visible 那份完全一致，只是用途不同
    D3D12_CPU_DESCRIPTOR_HANDLE clearHandle = m_clearHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateUnorderedAccessView(m_count.Get(), nullptr, &uavDesc, clearHandle);

    // ---- 5. SRV 视图：给 M12 的命令生成 CS 只读访问 ----
    //
    // 同一个资源可以有多个视图。这里额外建 SRV 而不是让 CS 通过 UAV 读，
    // 是因为只读访问能走纹理/只读缓存路径，而 UAV 访问必须绕过它。
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = maxInstances;
    srvDesc.Buffer.StructureByteStride = sizeof(std::uint32_t);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE indicesSrvHandle =
        descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    indicesSrvHandle.ptr += static_cast<SIZE_T>(indicesSrvSlot) * descriptorSize;
    device->CreateShaderResourceView(m_indices.Get(), &srvDesc, indicesSrvHandle);

    // 计数器用 RAW SRV（与它的 UAV 视图对应）
    srvDesc.Format = DXGI_FORMAT_R32_UINT;
    srvDesc.Buffer.NumElements = 1;
    srvDesc.Buffer.StructureByteStride = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE countSrvHandle =
        descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    countSrvHandle.ptr += static_cast<SIZE_T>(countSrvSlot) * descriptorSize;
    device->CreateShaderResourceView(m_count.Get(), &srvDesc, countSrvHandle);

    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE VisibleInstanceList::GetCountClearCpuHandle() const
{
    return m_clearHeap->GetCPUDescriptorHandleForHeapStart();
}

void VisibleInstanceList::TransitionIndicesTo(ID3D12GraphicsCommandList* cmd,
                                              D3D12_RESOURCE_STATES newState)
{
    TransitionResource(cmd, m_indices.Get(), m_indicesState, newState);
}

void VisibleInstanceList::TransitionCountTo(ID3D12GraphicsCommandList* cmd,
                                            D3D12_RESOURCE_STATES newState)
{
    TransitionResource(cmd, m_count.Get(), m_countState, newState);
}
