#include "App.h"
#include "Renderer.h"

#include <cwchar>

namespace {
    const wchar_t* DiffuseModeName(DiffuseGiMode mode)
    {
        switch (mode) {
        case DiffuseGiMode::DDGI: return L"DDGI";
        case DiffuseGiMode::SSGI: return L"SSGI";
        case DiffuseGiMode::RayTraced: return L"Ray Traced";
        case DiffuseGiMode::None: return L"Off";
        }
        return L"Unknown";
    }

    const wchar_t* ViewName(const RenderOptions& options)
    {
        if (options.legacyDebugMode != 0)
            return L"Diagnostic";
        switch (options.display) {
        case GiDisplayMode::Composite: return L"Composite";
        case GiDisplayMode::DiffuseOnly: return L"Diffuse Only";
        case GiDisplayMode::SpecularOnly: return L"Specular Only";
        }
        return L"Unknown";
    }
}

// 在 Renderer 完整定义可见的编译单元中生成 App 的构造与析构。
App::App() = default;
App::~App() = default;

void App::init(HWND hwnd)
{
    m_hwnd = hwnd;
    m_lastFrameTime = std::chrono::high_resolution_clock::now();
    // 宽高比与 Renderer 的渲染尺寸对应，位置与目标点使用世界坐标。
    m_camera.Init({ 0.0f, 1.3f, -5.5f }, { 0.0f, 1.1f, 0.0f }, 1920.0f / 1080.0f);

    m_renderer = std::make_unique<Renderer>(m_scene);
    m_renderer->Init(hwnd);
    // Scene::Build 填充物体列表后，再用当前场景中两个球的索引绑定演示控制器。
    m_demoController.Init(&m_scene, 6, 5);
}

void App::Shutdown()
{
    if (m_renderer) {
        // Renderer 先等待队列并清空图登记，reset 随后按成员析构顺序释放资源。
        m_renderer->Shutdown();
        m_renderer.reset();
    }
}

void App::RenderTick()
{
    const auto tickStart = std::chrono::high_resolution_clock::now();
    float dt = std::chrono::duration<float>(tickStart - m_lastFrameTime).count();
    m_lastFrameTime = tickStart;
    // 遇到断点或卡顿时限制单步推进的时间，避免相机和演示物体突然跳远。
    if (dt > 0.1f) {
        dt = 0.1f;
    }

    // 标题统计使用当前帧间隔和此前已返回的 Renderer 帧计时。
    UpdateFrameStats(dt);
    // 一帧只消费一次累计的右键拖动量。按键状态由 Win32 事件持续维护。
    m_camera.Update(dt, BuildCameraControl());
    m_input.ResetMouseDelta();

    const auto selectDiffuse = [this](DiffuseGiMode mode, GiDisplayMode display) {
        m_renderOptions.diffuse = mode;
        m_renderOptions.display = display;
        m_renderOptions.legacyDebugMode = 0;
        };
    const auto selectSpecular = [this](SpecularGiMode mode) {
        m_renderOptions.specular = mode;
        if (m_renderOptions.legacyDebugMode != 0) {
            m_renderOptions.display = GiDisplayMode::Composite;
            m_renderOptions.legacyDebugMode = 0;
        }
        };
    const auto selectDisplay = [this](GiDisplayMode display) {
        m_renderOptions.display = display;
        m_renderOptions.legacyDebugMode = 0;
        };

    // 1–4 沿用现有快捷键。F1–F4 选择漫反射算法，F5/F6 独立控制镜面反射。
    if (m_input.IsKeyDown('1')) selectDiffuse(DiffuseGiMode::DDGI, GiDisplayMode::Composite);
    if (m_input.IsKeyDown('2')) selectDiffuse(DiffuseGiMode::RayTraced, GiDisplayMode::Composite);
    if (m_input.IsKeyDown('3')) selectDiffuse(DiffuseGiMode::DDGI, GiDisplayMode::DiffuseOnly);
    if (m_input.IsKeyDown('4')) selectDiffuse(DiffuseGiMode::RayTraced, GiDisplayMode::DiffuseOnly);
    if (m_input.IsKeyDown(VK_F1)) selectDiffuse(DiffuseGiMode::DDGI, GiDisplayMode::Composite);
    if (m_input.IsKeyDown(VK_F2)) selectDiffuse(DiffuseGiMode::SSGI, GiDisplayMode::Composite);
    if (m_input.IsKeyDown(VK_F3)) selectDiffuse(DiffuseGiMode::RayTraced, GiDisplayMode::Composite);
    if (m_input.IsKeyDown(VK_F4)) selectDiffuse(DiffuseGiMode::None, GiDisplayMode::Composite);
    if (m_input.IsKeyDown(VK_F5)) selectSpecular(SpecularGiMode::None);
    if (m_input.IsKeyDown(VK_F6)) selectSpecular(SpecularGiMode::SSR);
    if (m_input.IsKeyDown(VK_F7)) selectDisplay(GiDisplayMode::SpecularOnly);
    if (m_input.IsKeyDown(VK_F8)) selectDisplay(GiDisplayMode::Composite);

    // 诊断编号继续对应 ShadersLighting.hlsl 中的调试分支。
    if (m_input.IsKeyDown('N')) m_renderOptions.legacyDebugMode = 2;   // 法线可视化。
    if (m_input.IsKeyDown('5')) m_renderOptions.legacyDebugMode = 11;  // 相邻探针中的有效数量。
    if (m_input.IsKeyDown('6')) m_renderOptions.legacyDebugMode = 12;  // 加权平均的 Chebyshev 可见性。
    if (m_input.IsKeyDown('7')) m_renderOptions.legacyDebugMode = 13;  // 相邻探针的总权重。
    if (m_input.IsKeyDown('8')) m_renderOptions.legacyDebugMode = 14;  // 权重最大的探针。
    if (m_input.IsKeyDown('9')) m_renderOptions.legacyDebugMode = 8;   // 乘以材质反照率前的采样辐照度。
    if (m_input.IsKeyDown('0')) m_renderOptions.legacyDebugMode = 9;   // 辐照度图集及 tile 边界。
    if (m_input.IsKeyDown('P')) m_renderOptions.legacyDebugMode = 10;  // 距离矩图集。
    if (m_input.IsKeyDown('T')) m_renderOptions.legacyDebugMode = 15;  // 仅使用三线性权重的采样辐照度。
    if (m_input.IsKeyDown('V')) m_renderOptions.legacyDebugMode = 19;  // 探针位置。

    // 先更新 CPU 物体变换，Renderer 等到帧槽可重用后再上传该帧的数据。
    m_demoController.Update(dt, m_input);
    // 按住 R 的每一帧都会请求重置 DDGI 历史。
    m_renderOptions.resetProbeHistory = m_input.IsKeyDown('R');
    const FrameTimings timings = m_renderer->RenderFrame(m_camera, dt, m_renderOptions);
    ++m_recordCpuSamples;
    m_waitMsAccum += timings.waitMs;
    m_beginMsAccum += timings.beginMs;
    m_recordCpuMsAccum += timings.recordMs;
    m_executeMsAccum += timings.executeMs;
    m_presentMsAccum += timings.presentMs;
    m_waitHitCount += timings.waitHit;
    m_usingGpuUpload = timings.usingGpuUpload;
    // TickCPU 从输入更新前开始计时，包含 Renderer 提交和 Present 在 CPU 侧的耗时。
    const auto tickEnd = std::chrono::high_resolution_clock::now();
    m_tickCpuMsAccum += std::chrono::duration<float, std::milli>(tickEnd - tickStart).count();
}


void App::UpdateFrameStats(float deltaTime) {
    m_fpsFrameCount++;
    m_fpsTimeElapsed += deltaTime;

    // 每累计约四分之一秒刷新一次，减少修改窗口标题的频率。
    if (m_fpsTimeElapsed >= 0.25f) {
        float avgFps = static_cast<float>(m_fpsFrameCount) / m_fpsTimeElapsed;
        float avgMs = (m_fpsTimeElapsed * 1000.0f) / static_cast<float>(m_fpsFrameCount);

        // 渲染阶段的平均值以已经返回计时的帧数为分母。
        const float samples = static_cast<float>(m_recordCpuSamples);
        const float invSamples = (samples > 0.0f) ? (1.0f / samples) : 0.0f;

        // WaitGPU 表示 CPU 等待帧槽 fence 的时间，Begin 已覆盖这段等待。
        // Execute 和 Present 记录 CPU 调用耗时，GPU 执行耗时需通过时间戳查询获得。
        const float avgWaitMs = m_waitMsAccum * invSamples;
        const float avgBeginMs = m_beginMsAccum * invSamples;
        const float avgRecordMs = m_recordCpuMsAccum * invSamples;
        const float avgExecuteMs = m_executeMsAccum * invSamples;
        const float avgPresentMs = m_presentMsAccum * invSamples;
        const float avgTickMs = m_tickCpuMsAccum * invSamples;

        wchar_t titleBuffer[1024];   // 给显卡名称与各阶段统计留出空间。
        swprintf_s(
            titleBuffer,
            L"Diffuse GI: %ls | Specular GI: %ls | View: %ls"
            L" | GPU: %ls | FPS: %.1f | Frame: %.3f ms"
            L" | WaitGPU: %.2f ms | WaitHit: %u/%u"
            L" | Begin: %.2f ms"
            L" | Record: %.2f ms | Execute: %.2f ms | Present: %.2f ms"
            L" | TickCPU: %.2f ms | Objects: %u | Heap: %ls",
            DiffuseModeName(m_renderOptions.diffuse),
            m_renderOptions.specular == SpecularGiMode::SSR ? L"SSR" : L"Off",
            ViewName(m_renderOptions),
            m_renderer->GetAdapterName(),               // 1  实际使用的显卡
            avgFps,                                   // 2  FPS
            avgMs,                                    // 3  Frame
            avgWaitMs,                                // 4  WaitGPU
            static_cast<unsigned>(m_waitHitCount),    // 5  WaitHit 分子
            static_cast<unsigned>(m_recordCpuSamples),// 6  WaitHit 分母
            avgBeginMs,                               // 7  Begin
            avgRecordMs,                              // 8  Record
            avgExecuteMs,                             // 9  Execute
            avgPresentMs,                             // 10 Present
            avgTickMs,                                // 11 TickCPU
            static_cast<unsigned>(m_scene.GetObjects().size()), // 12 Objects
            m_usingGpuUpload
            ? L"GPU_UPLOAD (ReBAR)" : L"UPLOAD (SysRAM)"); // 13 Heap

        SetWindowTextW(m_hwnd, titleBuffer);

        // 输出本周期统计后，帧数、总时间和等待次数从零重新累计。
        m_fpsFrameCount = 0;
        m_fpsTimeElapsed = 0.0f;
        m_recordCpuSamples = 0;
        m_recordCpuMsAccum = 0.0f;
        m_waitMsAccum = 0.0f;
        m_beginMsAccum = 0.0f;
        m_executeMsAccum = 0.0f;
        m_presentMsAccum = 0.0f;
        m_tickCpuMsAccum = 0.0f;
        m_waitHitCount = 0;
    }
}

CameraControl App::BuildCameraControl() const {
    CameraControl c;
    // 相对方向的按键相减，两个方向同时按住时该轴保持静止。
    c.moveForward = (m_input.IsKeyDown('W') ? 1.0f : 0.0f) - (m_input.IsKeyDown('S') ? 1.0f : 0.0f);
    c.moveRight = (m_input.IsKeyDown('D') ? 1.0f : 0.0f) - (m_input.IsKeyDown('A') ? 1.0f : 0.0f);
    c.moveUp = (m_input.IsKeyDown('E') ? 1.0f : 0.0f) - (m_input.IsKeyDown('Q') ? 1.0f : 0.0f);
    // Camera::Update 读取当前帧的像素拖动量，调用方随后清空累计值。
    c.lookDeltaX = m_input.GetMouseDeltaX();
    c.lookDeltaY = m_input.GetMouseDeltaY();
    return c;
}
