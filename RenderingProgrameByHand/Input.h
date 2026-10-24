#pragma once
#pragma once
#include <windows.h>

// 只做两件事：记录按键状态、累计鼠标拖动增量
class Input {
public:
    void OnKeyDown(WPARAM key);
    void OnKeyUp(WPARAM key);
    void OnMouseMove(int x, int y);
    void OnMouseButtonDown(int x, int y);
    void OnMouseButtonUp();
    void OnFocusLost();          // 失焦时清空，防止按键卡住

    bool IsKeyDown(int vk) const;

    float GetMouseDeltaX() const noexcept { return m_mouseDeltaX; }
    float GetMouseDeltaY() const noexcept { return m_mouseDeltaY; }
    void  ResetMouseDelta() noexcept { m_mouseDeltaX = 0.0f; m_mouseDeltaY = 0.0f; }

private:
    bool  m_keys[256]{};
    bool  m_mouseButtonDown = false;
    int   m_lastMouseX = 0;
    int   m_lastMouseY = 0;
    float m_mouseDeltaX = 0.0f;
    float m_mouseDeltaY = 0.0f;
};