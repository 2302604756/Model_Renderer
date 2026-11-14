#pragma once
#include "SceneTypes.h"

class Scene;
class Input;

// 阶段 1：演示用物体移动
// 小球绕圈；后墙前后滑动；可键盘控制
class DemoController {
public:
    void Init(Scene* scene, uint32_t smallSphereIndex, uint32_t wallIndex);
    void Update(float dt, const Input& input);

private:
    Scene* m_scene = nullptr;           // scene与controller一一绑定

    uint32_t m_smallSphereIndex = 0;    //两个现阶段可移动物体ID
    uint32_t m_wallIndex = 0;

    DirectX::XMFLOAT3 m_smallBase{};
    DirectX::XMFLOAT3 m_wallBase{};

    float m_time = 0.0f;
    bool  m_auto = false;
    bool  m_spaceLast = false;
};