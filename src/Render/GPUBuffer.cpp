#include "Render/GPUBuffer.h"

bool GPUBuffer::Initialize(ID3D12Device* device, std::uint64_t sizeBytes,
                           D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES initialState)
{
    m_sizeBytes = sizeBytes;
    m_heapType = heapType;
    m_state = initialState;

    // Heap 属性：决定内存在哪、CPU/GPU 如何访问
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = heapType;

    // Buffer 资源描述：一条连续的字节序列
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeBytes;      // buffer 的「宽度」即字节数
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    return SUCCEEDED(device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        initialState, nullptr, IID_PPV_ARGS(&m_resource)));
}

void* GPUBuffer::Map(UINT subresource, D3D12_RANGE* readRange)
{
    void* mapped = nullptr;
    if (FAILED(m_resource->Map(subresource, readRange, &mapped)))
    {
        return nullptr;
    }
    return mapped;
}

void GPUBuffer::Unmap(UINT subresource, const D3D12_RANGE* writtenRange)
{
    m_resource->Unmap(subresource, writtenRange);
}
