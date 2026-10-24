#pragma once
#include <DirectXMath.h>

// 相机每帧收到的"控制信号"：谁产生的都行（键鼠/手柄/回放/AI）
struct CameraControl {
    float moveForward = 0.0f;   // -1..1（W/S）
    float moveRight = 0.0f;   // -1..1（D/A）
    float moveUp = 0.0f;   // -1..1（E/Q）
    float lookDeltaX = 0.0f;   // 鼠标像素增量（右为正）
    float lookDeltaY = 0.0f;   // 鼠标像素增量（下为正）
};

class Camera {
public:
    // position: 相机位置；target: 初始看向的点；aspect: 宽/高
    void Init(const DirectX::XMFLOAT3& position,
        const DirectX::XMFLOAT3& target,
        float aspect,
        float fovY = DirectX::XMConvertToRadians(60.0f),
        float nearZ = 0.1f,
        float farZ = 100.0f);

    // 每帧：鼠标增量改朝向，WASD/QE 平移
    void Update(float dt, const CameraControl& ctl); 
    void SetAspectRatio(float aspect);             

    DirectX::XMMATRIX GetView() const;
    DirectX::XMMATRIX GetProj() const;
    DirectX::XMFLOAT3 GetPosition() const noexcept { return m_position; }

private:
    void RebuildView();

    DirectX::XMFLOAT3 m_position{ 0.0f, 0.0f, 0.0f };
    float m_yaw = 0.0f;   // 绕 Y 轴，0 = 朝向 +Z（LH）
    float m_pitch = 0.0f;   // 上下，正值抬头

    float m_fovY = 0.0f;
    float m_aspect = 1.0f;
    float m_nearZ = 0.1f;
    float m_farZ = 100.0f;

    DirectX::XMFLOAT4X4 m_view{};
    DirectX::XMFLOAT4X4 m_proj{};

    float m_moveSpeed = 2.0f;    // 单位/秒
    float m_mouseSensitivity = 0.003f;  // 弧度/像素（约 0.17°/像素）
};