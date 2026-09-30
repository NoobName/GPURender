#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <vector>

#include "Asset/TgaLoader.h" // ImageData

// GPU 纹理：一张 2D 纹理 + 它在 shader-visible SRV 描述符堆中的槽位号。
//
// 资源说明：
//   用途    ：给像素着色器提供基础颜色贴图。
//   Heap    ：纹理本体在 **Default Heap（显存）**；像素先写进 Upload Heap 的 staging，
//             再用 CopyTextureRegion 拷进显存（这是「纹理驻留显存」的标准做法）。
//   状态    ：初始 COPY_DEST，上传后转换为 PIXEL_SHADER_RESOURCE。
//   生命周期：与 Renderer 同寿命。
//   同步    ：与 Mesh 一样，staging 由调用方持有到 fence 等待之后。
//
// 与缓冲上传的最大区别：纹理每行起始地址必须按 256 字节对齐
// （D3D12_TEXTURE_DATA_PITCH_ALIGNMENT），所以要走 CopyTextureRegion 的
// PLACED_FOOTPRINT 路径，并逐行拷贝。
class Texture
{
public:
    bool Initialize(ID3D12Device* device,
                    ID3D12GraphicsCommandList* cmd,
                    const ImageData& image,
                    ID3D12DescriptorHeap* srvHeap,
                    UINT srvDescriptorSize,
                    UINT srvIndex,
                    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& stagingOut);

    ID3D12Resource* GetResource() const { return m_resource.Get(); }
    UINT GetSrvIndex() const { return m_srvIndex; }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    UINT m_srvIndex = 0;
};
