#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <vector>

#include <DirectXMath.h>

#include "Render/GPUBuffer.h"

// 极简调试线框渲染器：把 CPU 侧生成的线段画成 GPU 线框。
//
// 用途（M8）：可视化 **视锥（Frustum）** 与实例的 **包围球（Bounding Sphere）**，
// 用来直观验证剔除判断是否正确 —— 如果看到某个球明显在视锥外却被判为可见
// （或反之），就能立刻定位到数学上的错误。
//
// 实现要点：
//   - 顶点 = 世界空间位置 + 颜色（28 字节），每帧在 CPU 侧重建后整体上传；
//   - **复用网格的根签名**（只有 CBV b0），只换 PSO：
//     拓扑改为 LINELIST、关闭深度测试与写入；
//   - 关闭深度让线框永远可见：调试可视化的第一诉求是「看得见」，
//     被模型挡住一半的线框没有调试价值。
class DebugLines
{
public:
    // 顶点上限：足够画一个视锥 + 上百个包围球
    static constexpr UINT kMaxVertices = 120000;

    bool Initialize(ID3D12Device* device);

    void Begin();
    void AddLine(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b,
                 const DirectX::XMFLOAT4& color);
    void AddSphere(const DirectX::XMFLOAT4& sphere, const DirectX::XMFLOAT4& color,
                   UINT segments = 12);
    // 传入 viewProj 的逆矩阵：用 NDC 的 8 个角点反算出视锥在世界空间的 8 个顶点
    void AddFrustum(DirectX::FXMMATRIX inverseViewProj, const DirectX::XMFLOAT4& color);
    bool End();

    void Render(ID3D12GraphicsCommandList* cmd) const;
    bool HasContent() const { return !m_vertices.empty(); }

private:
    struct DebugVertex
    {
        DirectX::XMFLOAT3 position; // offset 0
        DirectX::XMFLOAT4 color;    // offset 12
    };
    static_assert(sizeof(DebugVertex) == 28,
                  "DebugVertex 必须紧凑占 28 字节，与 Input Layout 的 0/12 偏移一致");

    GPUBuffer m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView = {};
    std::vector<DebugVertex> m_vertices;
};
