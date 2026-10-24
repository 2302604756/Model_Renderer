#include "Input.h"

void Input::OnKeyDown(WPARAM key) { if (key < 256) m_keys[key] = true; }
void Input::OnKeyUp(WPARAM key) { if (key < 256) m_keys[key] = false; }

bool Input::IsKeyDown(int vk) const {
    return vk >= 0 && vk < 256 && m_keys[vk];
}

void Input::OnMouseMove(int x, int y) {
    if (m_mouseButtonDown) {          // 只有按住右键拖动时才产生转向增量
        m_mouseDeltaX += static_cast<float>(x - m_lastMouseX);
        m_mouseDeltaY += static_cast<float>(y - m_lastMouseY);
    }
    m_lastMouseX = x;
    m_lastMouseY = y;
}

void Input::OnMouseButtonDown(int x, int y) {
    m_mouseButtonDown = true;
    m_lastMouseX = x;
    m_lastMouseY = y;
    m_mouseDeltaX = m_mouseDeltaY = 0.0f;
}

void Input::OnMouseButtonUp() { m_mouseButtonDown = false; }

void Input::OnFocusLost() {
    for (bool& k : m_keys) k = false;
    m_mouseButtonDown = false;
    m_mouseDeltaX = m_mouseDeltaY = 0.0f;
}