#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <vector>

#include "Asset/MeshData.h"
#include "Render/GPUBuffer.h"

// GPU 侧的网格：顶点缓冲 + 索引缓冲，以及绘制所需的两个 View。
//
// 资源说明（对应项目规则「每个资源都要说清用途/Heap/状态/生命周期/同步」）：
//   用途    ：保存一个网格的顶点与索引，供 IASetVertexBuffers / IASetIndexBuffer 绑定。
//   Heap    ：两者都是 Default Heap（显存），因为创建后不再修改，只需 GPU 读取。
//   状态    ：顶点缓冲最终为 VERTEX_AND_CONSTANT_BUFFER，索引缓冲最终为 INDEX_BUFFER；
//             上传过程中会临时经过 COPY_DEST（见 UploadHelper）。
//   生命周期：与 Renderer 同寿命，存在 Renderer 的成员里。
//   同步    ：上传命令提交后由 Renderer 统一等待 fence，之后才释放 staging 资源。
class Mesh
{
public:
    // 创建缓冲并记录上传命令。
    // stagingOut 用来收集本次使用的 Upload Heap 临时资源 ——
    // 它们必须活到 GPU 执行完这批上传命令之后，所以由调用方统一持有并释放。
    bool Initialize(ID3D12Device* device,
                    ID3D12GraphicsCommandList* cmd,
                    const MeshData& data,
                    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& stagingOut);

    const D3D12_VERTEX_BUFFER_VIEW& GetVertexBufferView() const { return m_vertexBufferView; }
    const D3D12_INDEX_BUFFER_VIEW& GetIndexBufferView() const { return m_indexBufferView; }
    UINT GetIndexCount() const { return m_indexCount; }

    // M18：Mesh Shader 通过 SRV 读顶点（没有 Input Assembler），
    // 所以需要暴露底层资源来建视图。绑定用的 VBV 仍然保留给传统路径。
    ID3D12Resource* GetVertexBufferResource() const { return m_vertexBuffer.GetResource(); }

private:
    GPUBuffer m_vertexBuffer;
    GPUBuffer m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView = {};
    D3D12_INDEX_BUFFER_VIEW m_indexBufferView = {};
    UINT m_indexCount = 0;
};
