#include "Render/InstanceBuffer.h"

#include <cstring>
#include <iostream>

bool InstanceBuffer::Initialize(ID3D12Device* device,
                                ID3D12GraphicsCommandList* cmd,
                                std::uint32_t maxInstances,
                                const std::vector<InstanceData>& instances,
                                ID3D12DescriptorHeap* srvHeap,
                                UINT srvDescriptorSize,
                                UINT srvIndex,
                                std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& stagingOut)
{
    if (maxInstances == 0)
    {
        std::cerr << "[InstanceBuffer] maxInstances must be > 0\n";
        return false;
    }
    if (instances.size() > maxInstances)
    {
        std::cerr << "[InstanceBuffer] initial data exceeds capacity\n";
        return false;
    }

    m_capacity = maxInstances;
    m_srvIndex = srvIndex;

    // ---- 1. 创建 DEFAULT Heap 的 StructuredBuffer ----
    //
    // 注意：Buffer 资源的 InitialState 会被运行时忽略（实际总是 COMMON），
    // 所以这里写 COMMON，由 UploadInternal 插入 COMMON -> COPY_DEST 的屏障。
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    // 按最大实例数分配，这样切换场景规模时不需要重建资源、也不需要重建 SRV
    bufferDesc.Width = static_cast<UINT64>(m_capacity) * sizeof(InstanceData);
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // 后续里程碑会把这个缓冲作为 UAV 写入（剔除结果、压缩后的索引等），
    // 因此现在就把 ALLOW_UNORDERED_ACCESS 打开。
    bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_resource))))
    {
        std::cerr << "[InstanceBuffer] Failed to create DEFAULT heap buffer.\n";
        return false;
    }
    m_state = D3D12_RESOURCE_STATE_COMMON;

    // ---- 2. 创建 SRV（StructuredBuffer 视图）----
    //
    // 关键字段：
    //   StructureByteStride = sizeof(InstanceData) = 96
    //     -> 这就是 HLSL 侧读第二个元素时的步长。它与 HLSL 的
    //        sizeof(InstanceData) 必须一致，否则数据整体错位。
    //   NumElements = capacity
    //     -> 缓冲能容纳多少个元素（不是当前实际有多少个）。
    //        实际数量由 dispatch 的线程数控制。
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN; // StructuredBuffer 必须用 UNKNOWN
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = m_capacity;
    srvDesc.Buffer.StructureByteStride = sizeof(InstanceData);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += static_cast<SIZE_T>(srvIndex) * srvDescriptorSize;
    device->CreateShaderResourceView(m_resource.Get(), &srvDesc, srvHandle);

    // ---- 3. 上传初始数据 ----
    if (!instances.empty())
    {
        return UploadInternal(device, cmd, instances, stagingOut);
    }
    return true;
}

bool InstanceBuffer::Upload(ID3D12Device* device,
                           ID3D12GraphicsCommandList* cmd,
                           const std::vector<InstanceData>& instances,
                           std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& stagingOut)
{
    if (instances.size() > m_capacity)
    {
        std::cerr << "[InstanceBuffer] data exceeds capacity (" << instances.size()
                  << " > " << m_capacity << ")\n";
        return false;
    }
    return UploadInternal(device, cmd, instances, stagingOut);
}

bool InstanceBuffer::UploadInternal(ID3D12Device* device,
                                    ID3D12GraphicsCommandList* cmd,
                                    const std::vector<InstanceData>& instances,
                                    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& stagingOut)
{
    m_instanceCount = static_cast<std::uint32_t>(instances.size());
    const UINT64 byteCount = static_cast<UINT64>(m_instanceCount) * sizeof(InstanceData);
    if (byteCount == 0)
    {
        return true;
    }

    // ---- 创建 Upload Heap staging 并写入数据 ----
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC stagingDesc = {};
    stagingDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    stagingDesc.Width = byteCount;
    stagingDesc.Height = 1;
    stagingDesc.DepthOrArraySize = 1;
    stagingDesc.MipLevels = 1;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> staging;
    if (FAILED(device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &stagingDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging))))
    {
        std::cerr << "[InstanceBuffer] Failed to create staging buffer.\n";
        return false;
    }

    D3D12_RANGE readRange = { 0, 0 }; // CPU 只写不读
    void* mapped = nullptr;
    if (FAILED(staging->Map(0, &readRange, &mapped)))
    {
        return false;
    }
    // 逐字节拷贝：这正是「C++ 布局 == GPU 布局」这个契约的兑现点。
    // 如果两边布局不一致，拷贝本身不会报错，但 GPU 读出来就是错位的 ——
    // 因此 M9 专门加了 InstanceValidator 在运行期把数据读回来比对。
    std::memcpy(mapped, instances.data(), static_cast<std::size_t>(byteCount));
    staging->Unmap(0, nullptr);

    // ---- 记录拷贝与状态转换 ----
    D3D12_RESOURCE_BARRIER toCopyDest = {};
    toCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopyDest.Transition.pResource = m_resource.Get();
    toCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCopyDest.Transition.StateBefore = m_state;
    toCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    cmd->ResourceBarrier(1, &toCopyDest);

    cmd->CopyBufferRegion(m_resource.Get(), 0, staging.Get(), 0, byteCount);

    // 终态：NON_PIXEL_SHADER_RESOURCE —— M9 里它只被 Compute Shader 读取。
    // （后续若要在顶点着色器里读，需要改成 ALL_SHADER_RESOURCE。）
    D3D12_RESOURCE_BARRIER toSrv = {};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource = m_resource.Get();
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cmd->ResourceBarrier(1, &toSrv);

    m_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    stagingOut.push_back(staging);
    return true;
}
