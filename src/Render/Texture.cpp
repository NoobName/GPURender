#include "Render/Texture.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

bool Texture::Initialize(ID3D12Device* device,
                         ID3D12GraphicsCommandList* cmd,
                         const ImageData& image,
                         ID3D12DescriptorHeap* srvHeap,
                         UINT srvDescriptorSize,
                         UINT srvIndex,
                         std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& stagingOut)
{
    if (image.width == 0 || image.height == 0 || image.pixels.empty())
    {
        return false;
    }

    m_srvIndex = srvIndex;

    // ---- 1. 纹理本体：Default Heap（显存）----
    // 初始状态设为 COPY_DEST，因为紧接着就要把 staging 中的像素拷进来。
    D3D12_HEAP_PROPERTIES textureHeap = {};
    textureHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = image.width;
    textureDesc.Height = image.height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1; // M6 不生成 mipmap（留给后续里程碑）
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    if (FAILED(device->CreateCommittedResource(
            &textureHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_resource))))
    {
        return false;
    }

    // ---- 2. Upload Heap 上的 staging 缓冲 ----
    // 关键点：纹理每一行的起始地址必须按 256 字节对齐，所以 staging 的行间距
    // 往往大于 width * 4，必须「逐行拷贝」而不能一次性 memcpy 整张图。
    const UINT rowPitch =
        (image.width * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
        ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    const UINT64 uploadSize = static_cast<UINT64>(rowPitch) * image.height;

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC stagingDesc = {};
    stagingDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    stagingDesc.Width = uploadSize;
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
        return false;
    }

    // ---- 3. CPU 写入像素 ----
    // Upload Heap 对 CPU 永久映射，Map 一次即可；CPU 只写不读，readRange 用 {0,0}。
    D3D12_RANGE readRange = { 0, 0 };
    void* mapped = nullptr;
    if (FAILED(staging->Map(0, &readRange, &mapped)))
    {
        return false;
    }
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        std::memcpy(
            static_cast<std::uint8_t*>(mapped) + static_cast<std::size_t>(y) * rowPitch,
            image.pixels.data() + static_cast<std::size_t>(y) * image.width * 4,
            static_cast<std::size_t>(image.width) * 4);
    }
    staging->Unmap(0, nullptr);

    // ---- 4. staging -> 纹理 ----
    // 目标用 SUBRESOURCE_INDEX（整张子资源），源用 PLACED_FOOTPRINT（描述 staging 里的排布）。
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = m_resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = staging.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = 0;
    source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    source.PlacedFootprint.Footprint.Width = image.width;
    source.PlacedFootprint.Footprint.Height = image.height;
    source.PlacedFootprint.Footprint.Depth = 1;
    source.PlacedFootprint.Footprint.RowPitch = rowPitch;

    cmd->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    // ---- 5. 状态转换 COPY_DEST -> PIXEL_SHADER_RESOURCE ----
    // 像素着色器要通过 SRV 读它，所以必须切换到「像素着色器可读」状态；
    // 否则 Debug Layer 会报状态不匹配。
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmd->ResourceBarrier(1, &barrier);

    // ---- 6. 创建 SRV ----
    // SRV（Shader Resource View）= 「让着色器只读地看待这块资源」的描述。
    // 资源本身只是一个 ID3D12Resource，着色器要怎么解释它（格式、维度、mip 范围）
    // 全部由 View 决定 —— 这就是 D3D12 必须显式创建 View 的原因。
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.PlaneSlice = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += static_cast<SIZE_T>(srvIndex) * srvDescriptorSize;
    device->CreateShaderResourceView(m_resource.Get(), &srvDesc, srvHandle);

    stagingOut.push_back(staging);
    return true;
}
