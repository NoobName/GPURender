#pragma once

#include <DirectXMath.h>

// 最小相机：持有视图参数与透视投影参数，按需生成 View / Projection 矩阵。
// 本阶段相机位置固定（不实现移动/旋转输入），由 Renderer 每帧读取矩阵。
class Camera
{
public:
    // 设置视图（eye 看向 target）
    void SetView(DirectX::XMFLOAT3 eye, DirectX::XMFLOAT3 target, DirectX::XMFLOAT3 up);
    // 设置透视投影（fovY 为角度制）
    void SetPerspective(float fovYDegrees, float aspectRatio, float nearZ, float farZ);

    DirectX::XMMATRIX GetView() const;
    DirectX::XMMATRIX GetProjection() const;

private:
    DirectX::XMFLOAT3 m_eye = { 0.0f, 3.0f, -5.0f };
    DirectX::XMFLOAT3 m_target = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 m_up = { 0.0f, 1.0f, 0.0f };
    float m_fovYDegrees = 60.0f;
    float m_aspectRatio = 1280.0f / 720.0f;
    float m_nearZ = 0.1f;
    float m_farZ = 100.0f;
};
