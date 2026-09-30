#include "Render/DebugText.h"

#include <cstring>
#include <iostream>

#include "Asset/TgaLoader.h"

bool DebugText::Initialize(ID3D12Device* device,
                           ID3D12GraphicsCommandList* cmd,
                           const std::string& fontAtlasPath,
                           ID3D12DescriptorHeap* srvHeap,
                           UINT srvDescriptorSize,
                           UINT srvIndex,
                           std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& stagingOut)
{
    // ---- 1. 字体图集 -> GPU 纹理 + SRV ----
    ImageData atlas;
    std::string error;
    if (!LoadTgaFromFile(fontAtlasPath, atlas, error))
    {
        std::cerr << "[DebugText] " << error << "\n";
        return false;
    }
    if (!m_fontTexture.Initialize(device, cmd, atlas, srvHeap, srvDescriptorSize,
                                  srvIndex, stagingOut))
    {
        std::cerr << "[DebugText] Failed to create font atlas texture.\n";
        return false;
    }

    // ---- 2. 动态顶点缓冲（Upload Heap）----
    // 每帧都会被 CPU 整体重写，所以放在 Upload Heap 上、创建后一直映射即可。
    const UINT bufferSize =
        kMaxCharacters * 6u * static_cast<UINT>(sizeof(UiVertex));
    if (!m_vertexBuffer.Initialize(device, bufferSize, D3D12_HEAP_TYPE_UPLOAD,
                                   D3D12_RESOURCE_STATE_GENERIC_READ))
    {
        std::cerr << "[DebugText] Failed to create UI vertex buffer.\n";
        return false;
    }

    m_vertexBufferView.BufferLocation = m_vertexBuffer.GetGPUVirtualAddress();
    m_vertexBufferView.StrideInBytes = sizeof(UiVertex);
    m_vertexBufferView.SizeInBytes = bufferSize;

    m_vertices.reserve(static_cast<std::size_t>(kMaxCharacters) * 6);
    return true;
}

void DebugText::Begin(UINT screenWidth, UINT screenHeight)
{
    m_screenWidth = screenWidth;
    m_screenHeight = screenHeight;
    m_color = { 1.0f, 1.0f, 1.0f, 1.0f }; // 每帧重置为不透明白色
    m_vertices.clear();
}

void DebugText::SetColor(float r, float g, float b, float a)
{
    m_color = { r, g, b, a };
}

void DebugText::AddText(float x, float y, float scale, const char* text)
{
    if (text == nullptr)
    {
        return;
    }

    constexpr float atlasWidth = static_cast<float>(kAtlasColumns * kCellWidth);
    constexpr float atlasHeight = static_cast<float>(kAtlasRows * kCellHeight);
    constexpr float invAtlasWidth = 1.0f / atlasWidth;
    constexpr float invAtlasHeight = 1.0f / atlasHeight;

    const float cellWidth = static_cast<float>(kCellWidth) * scale;
    const float cellHeight = static_cast<float>(kCellHeight) * scale;

    float penX = x;
    float penY = y;

    for (const char* p = text; *p != '\0'; ++p)
    {
        const unsigned char ch = static_cast<unsigned char>(*p);
        if (ch == '\n')
        {
            penX = x;
            penY += cellHeight;
            continue;
        }
        if (ch < kFirstCharacter || ch >= kFirstCharacter + kCharacterCount)
        {
            penX += cellWidth; // 不可打印字符：只推进光标
            continue;
        }
        if (m_vertices.size() + 6 > static_cast<std::size_t>(kMaxCharacters) * 6)
        {
            return; // 达到单帧上限，丢弃剩余文本
        }

        const UINT index = ch - kFirstCharacter;
        const UINT cellX = index % kAtlasColumns;
        const UINT cellY = index / kAtlasColumns;

        const float u0 = static_cast<float>(cellX * kCellWidth) * invAtlasWidth;
        const float v0 = static_cast<float>(cellY * kCellHeight) * invAtlasHeight;
        const float u1 = u0 + static_cast<float>(kCellWidth) * invAtlasWidth;
        const float v1 = v0 + static_cast<float>(kCellHeight) * invAtlasHeight;

        const float x0 = penX;
        const float y0 = penY;
        const float x1 = penX + cellWidth;
        const float y1 = penY + cellHeight;

        // 一个字符 = 两个三角形。不共享顶点，所以不需要索引缓冲。
        m_vertices.push_back({ { x0, y0 }, { u0, v0 }, m_color });
        m_vertices.push_back({ { x1, y0 }, { u1, v0 }, m_color });
        m_vertices.push_back({ { x1, y1 }, { u1, v1 }, m_color });
        m_vertices.push_back({ { x0, y0 }, { u0, v0 }, m_color });
        m_vertices.push_back({ { x1, y1 }, { u1, v1 }, m_color });
        m_vertices.push_back({ { x0, y1 }, { u0, v1 }, m_color });

        penX += cellWidth;
    }
}

void DebugText::AddText(float x, float y, float scale, const std::string& text)
{
    AddText(x, y, scale, text.c_str());
}

void DebugText::AddRect(float x, float y, float width, float height)
{
    if (m_vertices.size() + 6 > static_cast<std::size_t>(kMaxCharacters) * 6)
    {
        return;
    }

    constexpr float invAtlasWidth = 1.0f / static_cast<float>(kAtlasColumns * kCellWidth);
    constexpr float invAtlasHeight = 1.0f / static_cast<float>(kAtlasRows * kCellHeight);

    // 实心色块在最后一格，取该格中心点的 UV，四个角共用它即可得到纯色。
    const UINT index = kCharacterCount - 1;
    const UINT cellX = index % kAtlasColumns;
    const UINT cellY = index / kAtlasColumns;
    const float u = static_cast<float>(cellX * kCellWidth + kCellWidth / 2) * invAtlasWidth;
    const float v = static_cast<float>(cellY * kCellHeight + kCellHeight / 2) * invAtlasHeight;

    const float x1 = x + width;
    const float y1 = y + height;

    m_vertices.push_back({ { x, y }, { u, v }, m_color });
    m_vertices.push_back({ { x1, y }, { u, v }, m_color });
    m_vertices.push_back({ { x1, y1 }, { u, v }, m_color });
    m_vertices.push_back({ { x, y }, { u, v }, m_color });
    m_vertices.push_back({ { x1, y1 }, { u, v }, m_color });
    m_vertices.push_back({ { x, y1 }, { u, v }, m_color });
}

bool DebugText::End()
{
    if (m_vertices.empty())
    {
        return true;
    }

    D3D12_RANGE readRange = { 0, 0 }; // CPU 只写不读
    void* mapped = m_vertexBuffer.Map(0, &readRange);
    if (mapped == nullptr)
    {
        return false;
    }
    std::memcpy(mapped, m_vertices.data(), m_vertices.size() * sizeof(UiVertex));
    m_vertexBuffer.Unmap(0, nullptr);
    return true;
}

void DebugText::Render(ID3D12GraphicsCommandList* cmd) const
{
    if (m_vertices.empty())
    {
        return;
    }
    cmd->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    cmd->DrawInstanced(static_cast<UINT>(m_vertices.size()), 1, 0, 0);
}

DirectX::XMFLOAT4X4 DebugText::GetOrthoMatrix() const
{
    // 把「左上为原点、y 向下、单位是像素」的屏幕空间映射到 NDC：
    // 注意 bottom / top 是反的（bottom = 屏幕高度，top = 0），这正是 y 轴翻转的来源。
    const DirectX::XMMATRIX ortho = DirectX::XMMatrixOrthographicOffCenterLH(
        0.0f, static_cast<float>(m_screenWidth),
        static_cast<float>(m_screenHeight), 0.0f,
        0.0f, 1.0f);

    DirectX::XMFLOAT4X4 result;
    DirectX::XMStoreFloat4x4(&result, ortho);
    return result;
}
