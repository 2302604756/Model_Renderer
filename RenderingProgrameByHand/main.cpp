// Win32 桌面程序入口。创建主窗口、转发输入并在消息队列空闲时驱动 App。
// 通过 Visual Studio 项目编译，App 初始化 Renderer 及其 DDGI 模块。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "RenderContext.h"
#include "d3d12lite.h"
#include "App.h"

#include <windows.h>
#include <windowsx.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "Input.h"



#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

// -----------------------------------------------------------------------------
// 窗口初始客户区尺寸。交换链的渲染尺寸在 Renderer 中设定。
// -----------------------------------------------------------------------------
static constexpr uint32_t WINDOW_WIDTH = 1280;
static constexpr uint32_t WINDOW_HEIGHT = 720;
static constexpr uint32_t FRAME_COUNT = 2; // 入口保留的帧数常量，实际帧槽数量由 RenderContext 配置。

// -----------------------------------------------------------------------------
// WindowProc 和 WinMain 共用的应用实例
// -----------------------------------------------------------------------------
App g_app;



// -----------------------------------------------------------------------------
// Win32 消息回调。与应用有关的输入交给 App，其余消息使用系统默认处理。
// -----------------------------------------------------------------------------
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: {
        // 确认窗口重绘区域。后缓冲绘制由 RenderTick 提交给交换链。
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:  g_app.OnKeyDown(wParam);  return 0;
    case WM_KEYUP:    g_app.OnKeyUp(wParam);    return 0;
    case WM_KILLFOCUS: g_app.OnFocusLost();     return 0; // 失去焦点时清空输入，避免按键停留在按住状态。
    case WM_MOUSEMOVE: g_app.OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0;
    case WM_RBUTTONDOWN:
        g_app.OnMouseButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        SetCapture(hwnd);                  // 拖出窗口也能继续收到鼠标消息
        return 0;
    case WM_RBUTTONUP:
        g_app.OnMouseButtonUp();
        // 结束右键拖动，让后续鼠标消息按系统规则分发。
        ReleaseCapture();
        return 0;
    case WM_DESTROY:
        // 通知主循环退出，由 WinMain 在停止提交帧之后统一关停渲染器。
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

// -----------------------------------------------------------------------------
// 注册窗口类，建立应用生命周期并驱动消息循环。
// -----------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // 1. 注册窗口类
    WNDCLASSEX wc{};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"D3D12MinimalWindowClass";
    RegisterClassEx(&wc);

    // 计算包含标题栏和边框的窗口尺寸，起始位置交由系统选择。
    RECT windowRect = { 0, 0, static_cast<LONG>(WINDOW_WIDTH), static_cast<LONG>(WINDOW_HEIGHT) };
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindow(
        wc.lpszClassName,
        L"DirectX 12 Minimal Starter Template",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!hwnd) return -1;

    try {
        // 先完成 App 与 Renderer 初始化，再按系统给出的显示方式显示窗口。
        g_app.init(hwnd);
        ShowWindow(hwnd, nCmdShow);

        // 优先处理待处理的消息。队列空闲时执行一帧，收到 WM_QUIT 后退出。
        MSG msg{};
        while (msg.message != WM_QUIT) {
            if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            else {
                g_app.RenderTick();
            }
        }

        // 此后停止录制帧，等待已提交的 GPU 工作结束并释放渲染资源。
        g_app.Shutdown();
    }
    catch (const std::exception& e) {
        // 初始化或渲染失败时仍尝试关停。二次错误写调试日志，错误对话框保留最初的异常。
        try {
            g_app.Shutdown();
        }
        catch (...) {
            OutputDebugStringA("Renderer shutdown failed while handling an earlier error.\n");
        }
        MessageBoxA(nullptr, e.what(), "Error", MB_OK | MB_ICONERROR);

        return -1;
    }

    return 0;
}
