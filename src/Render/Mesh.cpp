#include "Render/Mesh.h"

#include "Render/UploadHelper.h"

bool Mesh::Initialize(ID3D12Device* device,
                      ID3D12GraphicsCommandList* cmd,
                      const MeshData& data,
                      std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& stagingOut)
{
    if (data.vertices.empty() || data.indices.empty())
    {
        return false;
    }

    const UINT vertexBytes = static_cast<UINT>(data.vertices.size() * sizeof(MeshVertex));
    const UINT indexBytes = static_cast<UINT>(data.indices.size() * sizeof(std::uint32_t));

    // 缓冲一律以 COMMON 状态创建。
    // 注意：**Buffer 资源的 InitialState 会被运行时忽略**，实际总是 COMMON，
    // 所以这里显式写 COMMON，再由 UploadHelper 插入 COMMON -> COPY_DEST 的屏障。
    if (!m_vertexBuffer.Initialize(device, vertexBytes, D3D12_HEAP_TYPE_DEFAULT,
                                   D3D12_RESOURCE_STATE_COMMON))
    {
        return false;
    }
    if (!m_indexBuffer.Initialize(device, indexBytes, D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_STATE_COMMON))
    {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> vertexStaging = UploadHelper::Upload(
        device, cmd, m_vertexBuffer, data.vertices.data(), vertexBytes,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    if (vertexStaging == nullptr)
    {
        return false;
    }
    stagingOut.push_back(vertexStaging);

    Microsoft::WRL::ComPtr<ID3D12Resource> indexStaging = UploadHelper::Upload(
        device, cmd, m_indexBuffer, data.indices.data(), indexBytes,
        D3D12_RESOURCE_STATE_INDEX_BUFFER);
    if (indexStaging == nullptr)
    {
        return false;
    }
    stagingOut.push_back(indexStaging);

    m_vertexBufferView.BufferLocation = m_vertexBuffer.GetGPUVirtualAddress();
    m_vertexBufferView.StrideInBytes = sizeof(MeshVertex);
    m_vertexBufferView.SizeInBytes = vertexBytes;

    m_indexBufferView.BufferLocation = m_indexBuffer.GetGPUVirtualAddress();
    m_indexBufferView.Format = DXGI_FORMAT_R32_UINT; // 32 位索引，兼容任意顶点数
    m_indexBufferView.SizeInBytes = indexBytes;

    m_indexCount = static_cast<UINT>(data.indices.size());
    return true;
}
