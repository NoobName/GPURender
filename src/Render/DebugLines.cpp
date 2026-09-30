#include "Render/DebugLines.h"

#include <cmath>
#include <cstring>
#include <iostream>

using namespace DirectX;

namespace
{
constexpr float kPi = 3.14159265358979323846f;
}

bool DebugLines::Initialize(ID3D12Device* device)
{
    const UINT bufferSize = kMaxVertices * static_cast<UINT>(sizeof(DebugVertex));
    if (!m_vertexBuffer.Initialize(device, bufferSize, D3D12_HEAP_TYPE_UPLOAD,
                                   D3D12_RESOURCE_STATE_GENERIC_READ))
    {
        std::cerr << "[DebugLines] Failed to create vertex buffer.\n";
        return false;
    }

    m_vertexBufferView.BufferLocation = m_vertexBuffer.GetGPUVirtualAddress();
    m_vertexBufferView.StrideInBytes = sizeof(DebugVertex);
    m_vertexBufferView.SizeInBytes = bufferSize;

    m_vertices.reserve(4096);
    return true;
}

void DebugLines::Begin()
{
    m_vertices.clear();
}

void DebugLines::AddLine(const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT4& color)
{
    if (m_vertices.size() + 2 > kMaxVertices)
    {
        return; // 达到上限就丢弃多余线段，不打断整帧
    }
    m_vertices.push_back({ a, color });
    m_vertices.push_back({ b, color });
}

void DebugLines::AddSphere(const XMFLOAT4& sphere, const XMFLOAT4& color, UINT segments)
{
    // 用三个互相正交的大圆来近似球体线框。
    // 比经纬线更好：分布均匀，不会在两极堆积大量重复线段。
    const float radius = sphere.w;
    const float step = 2.0f * kPi / static_cast<float>(segments);

    for (UINT i = 0; i < segments; ++i)
    {
        const float a0 = step * static_cast<float>(i);
        const float a1 = step * static_cast<float>(i + 1);

        const float cos0 = std::cos(a0);
        const float sin0 = std::sin(a0);
        const float cos1 = std::cos(a1);
        const float sin1 = std::sin(a1);

        // XY 平面上的大圆
        AddLine({ sphere.x + radius * cos0, sphere.y + radius * sin0, sphere.z },
                { sphere.x + radius * cos1, sphere.y + radius * sin1, sphere.z }, color);
        // YZ 平面上的大圆
        AddLine({ sphere.x, sphere.y + radius * cos0, sphere.z + radius * sin0 },
                { sphere.x, sphere.y + radius * cos1, sphere.z + radius * sin1 }, color);
        // ZX 平面上的大圆
        AddLine({ sphere.x + radius * sin0, sphere.y, sphere.z + radius * cos0 },
                { sphere.x + radius * sin1, sphere.y, sphere.z + radius * cos1 }, color);
    }
}

void DebugLines::AddFrustum(FXMMATRIX inverseViewProj, const XMFLOAT4& color)
{
    // D3D 的 NDC：x,y ∈ [-1,1]，z ∈ [0,1]（近平面 z = 0，远平面 z = 1）。
    // 用 viewProj 的逆矩阵把这 8 个角点反投影回世界空间，就得到视锥的 8 个顶点。
    static const float kCornerNdc[8][3] = {
        { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f }, { -1.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 1.0f }, { 1.0f, -1.0f, 1.0f },
        {  1.0f,  1.0f, 1.0f }, { -1.0f, 1.0f, 1.0f },
    };

    XMFLOAT3 world[8] = {};
    for (int i = 0; i < 8; ++i)
    {
        const XMVECTOR ndc = XMVectorSet(kCornerNdc[i][0], kCornerNdc[i][1],
                                         kCornerNdc[i][2], 1.0f);
        XMVECTOR position = XMVector4Transform(ndc, inverseViewProj);
        // 逆投影回来之后 w 不再是 1，必须自己做透视除法
        const float w = XMVectorGetW(position);
        position = XMVectorScale(position, 1.0f / w);
        XMStoreFloat3(&world[i], position);
    }

    for (int i = 0; i < 4; ++i)
    {
        const int next = (i + 1) % 4;
        AddLine(world[i], world[next], color);            // 近平面边
        AddLine(world[4 + i], world[4 + next], color);    // 远平面边
        AddLine(world[i], world[4 + i], color);           // 侧棱
    }
}

bool DebugLines::End()
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
    std::memcpy(mapped, m_vertices.data(), m_vertices.size() * sizeof(DebugVertex));
    m_vertexBuffer.Unmap(0, nullptr);
    return true;
}

void DebugLines::Render(ID3D12GraphicsCommandList* cmd) const
{
    if (m_vertices.empty())
    {
        return;
    }
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    cmd->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    cmd->DrawInstanced(static_cast<UINT>(m_vertices.size()), 1, 0, 0);
}
