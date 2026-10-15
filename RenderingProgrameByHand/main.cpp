// =============================================================================
// D3D12 Minimal Clean Starter Template (Single File, No External Dependencies)
// =============================================================================
// 编译说明 (Compile with MSVC):
//   cl.exe /std:c++17 /EHsc d3d12_minimal.cpp user32.lib gdi32.lib d3d12.lib dxgi.lib
// 或直接在 Visual Studio C++ Windows 桌面项目中加入本文件。
// =============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "RenderContext.h"
#include "d3d12lite.h"
#include "App.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>



#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

// -----------------------------------------------------------------------------
// 常量定义
// -----------------------------------------------------------------------------
static constexpr uint32_t WINDOW_WIDTH = 1280;
static constexpr uint32_t WINDOW_HEIGHT = 720;
static constexpr uint32_t FRAME_COUNT = 2; // 双重缓冲

// -----------------------------------------------------------------------------
// 全局 DX12 上下文与核心资源
// -----------------------------------------------------------------------------
App g_app; // 全局或静态 App 实例



// -----------------------------------------------------------------------------
// Win32 消息循环回调
// -----------------------------------------------------------------------------
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT:
        g_app.RenderTick();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

// -----------------------------------------------------------------------------
// 入口函数
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

    // 2. 创建窗口并居中
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
        // 3. 初始化 D3D12
        g_app.init(hwnd);
        ShowWindow(hwnd, nCmdShow);

        // 4. 实时渲染消息循环
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

       
    }
    catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Error", MB_OK | MB_ICONERROR);
        
        return -1;
    }

    return 0;
}
