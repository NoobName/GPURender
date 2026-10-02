#include "Render/MeshletResources.h"

#include <cstring>
#include <iostream>

namespace
{
// -----------------------------------------------------------------------------
// 把一个 CPU 数组上传成 DEFAULT Heap 的只读缓冲。
//
// 完整流程（每一步都不能省）：
//   1. 建 DEFAULT heap 的缓冲（GPU 侧真正的家），初始状态写 COMMON
//   2. 建同等大小的 UPLOAD staging（CPU 可写）
//   3. Map -> memcpy -> Unmap
//   4. barrier COMMON -> COPY_DEST
//   5. CopyBufferRegion 把 staging 拷进 DEFAULT
//   6. barrier COPY_DEST -> NON_PIXEL_SHADER_RESOURCE（此后再不变）
//
// 为什么 InitialState 写 COMMON 再显式转换：
//   D3D12 对**缓冲**会忽略 CreateCommittedResource 传来的 InitialState
//   （Debug Layer 会报 ID=1328）。传 COPY_DEST 只是换来一条警告，
//   真正生效的必须是后面那次显式 barrier —— M16 已经踩过一次。
//
// 为什么 staging 由调用方保管：
//   CopyBufferRegion 只是**记录**到命令列表里，GPU 还没执行。
//   如果这里就释放 staging，GPU 可能读到已释放内存（经典 use-after-free）。
//   所以统一收集到 stagingOut，由调用方等 fence 之后再释放。
// -----------------------------------------------------------------------------
bool CreateAndUploadBuffer(ID3D12Device* device,
                           ID3D12GraphicsCommandList* cmd,
                           const void* data,
                           UINT64 sizeBytes,
                           ID3D12Resource** outBuffer,
                           ComPtr<ID3D12Resource>& outStaging)
{
    if (sizeBytes == 0)
    {
        std::cerr << "[MeshletResources] Refusing to create a zero-sized buffer.\n";
        return false;
    }

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width              = sizeBytes;
    desc.Height             = 1;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.SampleDesc.Count   = 1;
    desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags              = D3D12_RESOURCE_FLAG_NONE; // 只读，不需要 UAV

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(outBuffer))))
    {
        std::cerr << "[MeshletResources] CreateCommittedResource (default) failed.\n";
        return false;
    }

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&outStaging))))
    {
        std::cerr << "[MeshletResources] CreateCommittedResource (staging) failed.\n";
        return false;
    }

    {
        void* mapped = nullptr;
        const D3D12_RANGE readRange = { 0, 0 }; // CPU 不读这块内存
        if (FAILED(outStaging->Map(0, &readRange, &mapped)))
        {
            std::cerr << "[MeshletResources] Failed to map staging buffer.\n";
            return false;
        }
        std::memcpy(mapped, data, static_cast<std::size_t>(sizeBytes));
        outStaging->Unmap(0, nullptr);
    }

    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = *outBuffer;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        cmd->ResourceBarrier(1, &barrier);
    }

    cmd->CopyBufferRegion(*outBuffer, 0, outStaging.Get(), 0, sizeBytes);

    {
        // 转到 SRV 状态后**永不再变** —— M17/M18 都不会写这四个缓冲，
        // 所以不需要每帧的状态转换，也不需要 UAV barrier。
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = *outBuffer;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cmd->ResourceBarrier(1, &barrier);
    }

    return true;
}

// 建一个 StructuredBuffer SRV（或 RAW SRV，stride 传 0 时）。
void CreateStructuredSrv(ID3D12Device* device,
                         ID3D12Resource* resource,
                         UINT numElements,
                         UINT stride,
                         ID3D12DescriptorHeap* heap,
                         UINT descriptorSize,
                         UINT slot)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    if (stride == 0)
    {
        // RAW（R32_UINT）视图：元素本身就是 uint32
        srvDesc.Format              = DXGI_FORMAT_R32_UINT;
        srvDesc.Buffer.NumElements  = numElements;
        srvDesc.Buffer.StructureByteStride = 0;
    }
    else
    {
        srvDesc.Format              = DXGI_FORMAT_UNKNOWN; // StructuredBuffer 必须用 UNKNOWN
        srvDesc.Buffer.NumElements  = numElements;
        srvDesc.Buffer.StructureByteStride = stride;
    }

    srvDesc.ViewDimension                   = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement             = 0;
    srvDesc.Buffer.Flags                    = D3D12_BUFFER_SRV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
    device->CreateShaderResourceView(resource, &srvDesc, handle);
}
} // namespace

bool MeshletResources::Initialize(ID3D12Device* device,
                                  ID3D12GraphicsCommandList* cmd,
                                  const MeshletBuilder& builder,
                                  ID3D12DescriptorHeap* descriptorHeap,
                                  UINT descriptorSize,
                                  UINT firstSrvSlot,
                                  std::vector<ComPtr<ID3D12Resource>>& stagingOut)
{
    if (!builder.IsBuilt())
    {
        std::cerr << "[MeshletResources] Builder has no meshlet data.\n";
        return false;
    }

    const std::vector<MeshletGPU>&            meshlets    = builder.GetMeshlets();
    const std::vector<std::uint32_t>&         vertexIdx   = builder.GetUniqueVertexIndices();
    const std::vector<MeshletTriGPU>&         primIdx     = builder.GetPrimitiveIndices();
    const std::vector<MeshletBoundsGPU>&      bounds      = builder.GetBounds();

    // ---- ① Meshlet 描述 ----------------------------------------------------
    const UINT64 meshletBytes = static_cast<UINT64>(meshlets.size()) * sizeof(MeshletGPU);
    ComPtr<ID3D12Resource> meshletStaging;
    if (!CreateAndUploadBuffer(device, cmd, meshlets.data(), meshletBytes,
                               &m_meshletBuffer, meshletStaging))
    {
        return false;
    }
    CreateStructuredSrv(device, m_meshletBuffer.Get(),
                        static_cast<UINT>(meshlets.size()), sizeof(MeshletGPU),
                        descriptorHeap, descriptorSize, firstSrvSlot + kMeshletSrvOffset);

    // ---- ② 局部槽位 -> 原始顶点索引 ----------------------------------------
    const UINT64 vertexIdxBytes = static_cast<UINT64>(vertexIdx.size()) * sizeof(std::uint32_t);
    ComPtr<ID3D12Resource> vertexStaging;
    if (!CreateAndUploadBuffer(device, cmd, vertexIdx.data(), vertexIdxBytes,
                               &m_vertexIndexBuffer, vertexStaging))
    {
        return false;
    }
    CreateStructuredSrv(device, m_vertexIndexBuffer.Get(),
                        static_cast<UINT>(vertexIdx.size()), sizeof(std::uint32_t),
                        descriptorHeap, descriptorSize, firstSrvSlot + kVertexIndexSrvOffset);

    // ---- ③ 三角形（10+10+10 位打包的局部索引）------------------------------
    const UINT64 primIdxBytes = static_cast<UINT64>(primIdx.size()) * sizeof(MeshletTriGPU);
    ComPtr<ID3D12Resource> primStaging;
    if (!CreateAndUploadBuffer(device, cmd, primIdx.data(), primIdxBytes,
                               &m_primIndexBuffer, primStaging))
    {
        return false;
    }
    CreateStructuredSrv(device, m_primIndexBuffer.Get(),
                        static_cast<UINT>(primIdx.size()), sizeof(MeshletTriGPU),
                        descriptorHeap, descriptorSize, firstSrvSlot + kPrimIndexSrvOffset);

    // ---- ④ 剔除数据（包围球 + 法线锥）--------------------------------------
    const UINT64 boundsBytes = static_cast<UINT64>(bounds.size()) * sizeof(MeshletBoundsGPU);
    ComPtr<ID3D12Resource> boundsStaging;
    if (!CreateAndUploadBuffer(device, cmd, bounds.data(), boundsBytes,
                               &m_boundsBuffer, boundsStaging))
    {
        return false;
    }
    CreateStructuredSrv(device, m_boundsBuffer.Get(),
                        static_cast<UINT>(bounds.size()), sizeof(MeshletBoundsGPU),
                        descriptorHeap, descriptorSize, firstSrvSlot + kBoundsSrvOffset);

    // staging 的生命周期交给调用方：必须等上传命令真正执行完才能释放。
    stagingOut.push_back(meshletStaging);
    stagingOut.push_back(vertexStaging);
    stagingOut.push_back(primStaging);
    stagingOut.push_back(boundsStaging);

    m_totalGpuBytes = meshletBytes + vertexIdxBytes + primIdxBytes + boundsBytes;

    std::cout << "[MeshletResources] GPU buffers uploaded:\n";
    std::cout << "        meshlets        : " << meshlets.size() << " x " << sizeof(MeshletGPU)
              << " B = " << meshletBytes << " B\n";
    std::cout << "        unique vert idx : " << vertexIdx.size() << " x 4 B = "
              << vertexIdxBytes << " B\n";
    std::cout << "        primitive idx   : " << primIdx.size() << " x 4 B = "
              << primIdxBytes << " B\n";
    std::cout << "        cull bounds     : " << bounds.size() << " x " << sizeof(MeshletBoundsGPU)
              << " B = " << boundsBytes << " B\n";
    std::cout << "        total           : " << (m_totalGpuBytes / 1024.0) << " KB\n";
    std::cout << "        SRV slots       : " << firstSrvSlot << " .. "
              << (firstSrvSlot + kSrvCount - 1) << "\n";

    return true;
}
