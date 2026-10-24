#include "Camera.h"


#include <cmath>
#include <algorithm>

using namespace DirectX;

void Camera::Init(const XMFLOAT3& position, const XMFLOAT3& target, float aspect,
    float fovY, float nearZ, float farZ) {

    m_position = position;
    m_fovY = fovY;
    m_aspect = aspect;
    m_nearZ = nearZ;
    m_farZ = farZ;

    // 由"看向目标"反推初始 yaw/pitch —— 这样调用方不用自己算角度
    XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&target) - XMLoadFloat3(&position));
    XMFLOAT3 d{};
    XMStoreFloat3(&d, dir);
    m_yaw = std::atan2(d.x, d.z);
    m_pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f));

    XMStoreFloat4x4(&m_proj, XMMatrixPerspectiveFovLH(m_fovY, m_aspect, m_nearZ, m_farZ));
    RebuildView();
}

void Camera::Update(float dt, const CameraControl& c) {
    // 鼠标（像素增量 × 灵敏度仍然留在相机里：手感是相机的事）
    m_yaw += c.lookDeltaX * m_mouseSensitivity;
    m_pitch -= c.lookDeltaY * m_mouseSensitivity;
    const float limit = XMConvertToRadians(89.0f);
    m_pitch = std::clamp(m_pitch, -limit, limit);

    // ---- 2. 由 yaw/pitch 构造方向向量（LH：+Z 是前方，+X 是右方）----
    const float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    XMVECTOR forward = XMVectorSet(sy * cp, sp, cy * cp, 0.0f);
    XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), forward));
    XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);

    // 用抽象轴代替按键判断
    XMVECTOR move = forward * c.moveForward + right * c.moveRight + worldUp * c.moveUp;
    if (XMVectorGetX(XMVector3LengthSq(move)) > 0.0f) {
        move = XMVector3Normalize(move) * (m_moveSpeed * dt);
        XMStoreFloat3(&m_position, XMLoadFloat3(&m_position) + move);
    }
    RebuildView();
}

void Camera::SetAspectRatio(float aspect) {
    m_aspect = aspect;
    XMStoreFloat4x4(&m_proj,
        XMMatrixPerspectiveFovLH(m_fovY, m_aspect, m_nearZ, m_farZ));
}

void Camera::RebuildView()
{
    const float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    XMVECTOR forward = XMVectorSet(sy * cp, sp, cy * cp, 0.0f);
    XMStoreFloat4x4(&m_view,
        XMMatrixLookToLH(XMLoadFloat3(&m_position), forward, XMVectorSet(0, 1, 0, 0)));
}

XMMATRIX Camera::GetView() const { return XMLoadFloat4x4(&m_view); }
XMMATRIX Camera::GetProj() const { return XMLoadFloat4x4(&m_proj); }