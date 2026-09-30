#include "Render/Camera.h"

using namespace DirectX;

void Camera::SetView(XMFLOAT3 eye, XMFLOAT3 target, XMFLOAT3 up)
{
    m_eye = eye;
    m_target = target;
    m_up = up;
}

void Camera::SetPerspective(float fovYDegrees, float aspectRatio, float nearZ, float farZ)
{
    m_fovYDegrees = fovYDegrees;
    m_aspectRatio = aspectRatio;
    m_nearZ = nearZ;
    m_farZ = farZ;
}

XMMATRIX Camera::GetView() const
{
    // 左手坐标系视图矩阵（DirectX 默认左手系）
    return XMMatrixLookAtLH(XMLoadFloat3(&m_eye),
                            XMLoadFloat3(&m_target),
                            XMLoadFloat3(&m_up));
}

XMMATRIX Camera::GetProjection() const
{
    // 左手透视投影（fov 角度制 -> 弧度）
    return XMMatrixPerspectiveFovLH(XMConvertToRadians(m_fovYDegrees),
                                    m_aspectRatio, m_nearZ, m_farZ);
}
