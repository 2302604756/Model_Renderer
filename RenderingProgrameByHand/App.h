#pragma once

#include "Camera.h"
#include "DemoController.h"
#include "GiTypes.h"
#include "Input.h"
#include "Scene.h"

#include <chrono>
#include <cstdint>
#include <memory>

class Renderer;

// 应用层接收 Win32 输入、更新相机和 CPU 场景，并把每帧渲染交给 Renderer。
class App {
public:
    // 构造和析构放在 App.cpp，保证销毁 unique_ptr 时 Renderer 已完整定义。
    App();
    ~App();

    // App 独占场景和渲染器。Renderer 与 DemoController 会借用场景。
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // WinMain 创建窗口后调用。Renderer 先构建场景，随后演示控制器绑定场景物体。
    void init(HWND hwnd);
    // 消息队列空闲时推进一帧，依次处理输入、更新场景和提交渲染。
    void RenderTick();
    // 退出消息循环后调用，先完成已提交的 GPU 工作，再销毁 Renderer。
    void Shutdown();

    // WindowProc 更新输入状态，RenderTick 在帧开始时读取这些状态。
    void OnKeyDown(WPARAM key) { m_input.OnKeyDown(key); }
    void OnKeyUp(WPARAM key) { m_input.OnKeyUp(key); }
    void OnFocusLost() { m_input.OnFocusLost(); }
    void OnMouseMove(int x, int y) { m_input.OnMouseMove(x, y); }
    void OnMouseButtonDown(int x, int y) { m_input.OnMouseButtonDown(x, y); }
    void OnMouseButtonUp() { m_input.OnMouseButtonUp(); }

private:
    // 将按键和鼠标拖动增量转换为 Camera 使用的控制量。
    CameraControl BuildCameraControl() const;
    // 达到统计周期后更新窗口标题，并清理这一周期的累计值。
    void UpdateFrameStats(float deltaTime);

    // 声明顺序保证借用方先于 Scene 析构。
    Scene m_scene;
    std::unique_ptr<Renderer> m_renderer;
    DemoController m_demoController;
    Camera m_camera;
    Input m_input;

    HWND m_hwnd = nullptr;
    RenderOptions m_renderOptions{}; // 分别选择漫反射、镜面算法和显示视图。
    std::chrono::high_resolution_clock::time_point m_lastFrameTime;

    // 相邻帧的 CPU 时间间隔用于统计标题中的 FPS 和平均帧耗时。
    uint32_t m_fpsFrameCount = 0;
    float m_fpsTimeElapsed = 0.0f;
    // 每次成功执行 RenderFrame 后累加一份 CPU 计时，WaitHit 记录发生帧槽等待的次数。
    uint32_t m_recordCpuSamples = 0;
    float m_recordCpuMsAccum = 0.0f;
    float m_waitMsAccum = 0.0f;
    float m_tickCpuMsAccum = 0.0f;
    float m_beginMsAccum = 0.0f;
    float m_executeMsAccum = 0.0f;
    float m_presentMsAccum = 0.0f;
    uint32_t m_waitHitCount = 0;
    bool m_usingGpuUpload = false; // 最近一帧使用的上传堆类型，用于标题显示。
};
