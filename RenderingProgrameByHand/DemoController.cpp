#include "DemoController.h"
#include "Scene.h"
#include "Input.h"
#include <DirectXMath.h>

void DemoController::Init(Scene* scene, uint32_t smallSphereIndex, uint32_t wallIndex)
{
    m_scene = scene;
    m_smallSphereIndex = smallSphereIndex;
    m_wallIndex = wallIndex;

    auto& objects = m_scene->GetMutableObjects();
    m_smallBase = objects[m_smallSphereIndex].position;
    m_wallBase = objects[m_wallIndex].position;
}


void DemoController::Update(float dt, const Input& input)
{
    if (!m_scene) return;

    auto& objects = m_scene->GetMutableObjects();
    if (objects.size() <= m_smallSphereIndex || objects.size() <= m_wallIndex)
        return;

    m_time += dt;

    // 空格切换自动/手动
    bool space = input.IsKeyDown(VK_SPACE);
    if (space && !m_spaceLast)
        m_auto = !m_auto;
    m_spaceLast = space;

    if (m_auto)
    {
        // 小球在初始位置附近绕圈
        float a = m_time * 1.2f;
        objects[m_smallSphereIndex].position.x = m_smallBase.x + DirectX::XMScalarSin(a) * 0.6f;
        objects[m_smallSphereIndex].position.z = m_smallBase.z + DirectX::XMScalarCos(a) * 0.6f;

        // 后墙前后滑动（幅度小，保持在房间/网格内）
        objects[m_wallIndex].position.z = m_wallBase.z + DirectX::XMScalarSin(m_time * 0.8f) * 0.15f;
    }
    else
    {
        const float speed = 1.2f * dt;

        if (input.IsKeyDown('J')) objects[m_smallSphereIndex].position.x -= speed;
        if (input.IsKeyDown('L')) objects[m_smallSphereIndex].position.x += speed;

        if (input.IsKeyDown('U')) objects[m_wallIndex].position.z -= speed;
        if (input.IsKeyDown('O')) objects[m_wallIndex].position.z += speed;
    }
}