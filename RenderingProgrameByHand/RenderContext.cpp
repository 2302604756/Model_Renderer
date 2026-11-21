#include "RenderContext.h"
#include <dxgi1_6.h>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <system_error>
#include "d3d12lite.h"

using namespace d3d12lite;

// Renderer::Shutdown 在正常关停时先排空队列。
// 这里释放 Win32 事件，成员持有的 COM 引用随后自动释放。
RenderContext::~RenderContext() noexcept {
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
    }
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

void RenderContext::Init(HWND hwnd, uint32_t width, uint32_t height) {
    // -------------------------------------------------------------------------
    // 1. 在 Debug 构建中尝试启用 D3D12 和 DXGI 的调试支持。
    // -------------------------------------------------------------------------
    uint32_t dxgiFactoryFlags = 0;
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif
    // -------------------------------------------------------------------------
    // 2. 创建 DXGI Factory 与 D3D12 物理设备
    // -------------------------------------------------------------------------

    THROW_IF_FAILED(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_factory)));

    // 设备选择依次尝试 NVIDIA 硬件、DXGI 高性能排序和常规枚举。
    // 每个候选适配器都必须通过硬件标记检查，并能创建 Feature Level 12.0 设备。
    auto tryCreateDevice = [&](IDXGIAdapter1* candidate) -> bool
        {
            DXGI_ADAPTER_DESC1 candidateDesc{};
            candidate->GetDesc1(&candidateDesc);

            if (candidateDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                return false;

            if (FAILED(D3D12CreateDevice(
                candidate,
                D3D_FEATURE_LEVEL_12_0,
                IID_PPV_ARGS(&m_device))))
            {
                return false;
            }

            // 记录实际选中的适配器，窗口标题会显示它
            m_adapterName = candidateDesc.Description;

            wchar_t logBuffer[512]{};
            swprintf_s(
                logBuffer,
                L"[RenderContext] Adapter: %s Vendor=0x%04X DedicatedVRAM=%llu MB\n",
                candidateDesc.Description,
                candidateDesc.VendorId,
                static_cast<unsigned long long>(candidateDesc.DedicatedVideoMemory >> 20));
            OutputDebugStringW(logBuffer);
            return true;
        };

    ComPtr<IDXGIAdapter1> adapter;

    // 1) 先尝试 VendorId 为 0x10DE 的适配器。
    for (uint32_t i = 0;
        m_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
        ++i)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);

        if (desc.VendorId == 0x10DE && tryCreateDevice(adapter.Get()))
            break;
    }

    // 2) 首轮未创建设备时，尝试 IDXGIFactory6 提供的高性能枚举。
    if (!m_device)
    {
        ComPtr<IDXGIFactory6> factory6;
        if (SUCCEEDED(m_factory.As(&factory6)))
        {
            for (uint32_t i = 0;; ++i)
            {
                ComPtr<IDXGIAdapter1> candidate;
                HRESULT hr = factory6->EnumAdapterByGpuPreference(
                    i,
                    DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    IID_PPV_ARGS(&candidate));

                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                if (FAILED(hr))
                    continue;

                if (tryCreateDevice(candidate.Get()))
                {
                    adapter = candidate;
                    break;
                }
            }
        }
    }

    // 3) 回退到常规枚举，采用第一个能创建设备的硬件适配器。
    if (!m_device)
    {
        for (uint32_t i = 0;
            m_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
            ++i)
        {
            if (tryCreateDevice(adapter.Get()))
                break;
        }
    }

    if (!m_device) {
        throw std::runtime_error("Failed to find a compatible Direct3D 12 physical device.");
    }
    // -------------------------------------------------------------------------
    // 3. 创建主 Direct 队列，供初始化上传、各渲染阶段和交换链共用。
    // -------------------------------------------------------------------------
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    THROW_IF_FAILED(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));

    // -------------------------------------------------------------------------
    // 4. 为应用窗口创建 FLIP_DISCARD 交换链，使用单采样后缓冲区。
    // -------------------------------------------------------------------------
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = m_maxFrameCount; // 缓冲区数与软件帧槽数相同
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;

    ComPtr<IDXGISwapChain1> swapChain1;
    THROW_IF_FAILED(m_factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(),
        hwnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain1
    ));

    // 禁用 DXGI 默认的 Alt+Enter 全屏切换。
    THROW_IF_FAILED(m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));
    THROW_IF_FAILED(swapChain1.As(&m_swapChain));

    //-------------------------------------------------------------------------
    // 5. 为每个在途帧分配独立的命令对象和上传空间。
    // -------------------------------------------------------------------------
    for (uint32_t i = 0; i < m_maxFrameCount; i++) {
        THROW_IF_FAILED(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_frameResources[i].cmdAllocator)
        ));
        // 命令列表使用这个帧槽独有的分配器。
        THROW_IF_FAILED(m_device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_frameResources[i].cmdAllocator.Get(),
            nullptr, // 初始 PSO 置空
            IID_PPV_ARGS(&m_frameResources[i].cmdList)
        ));
        // 新建命令列表已打开。先关闭，首次使用时再 Reset 进入录制状态。
        THROW_IF_FAILED(m_frameResources[i].cmdList->Close());

        m_frameResources[i].fenceValue = 0;

        // 每个槽位预留 32 MiB。UploadHeap 优先使用 GPU_UPLOAD，按设备能力回退到 UPLOAD。
        m_frameResources[i].frameUpload.Init(32 * 1024 * 1024, true);
    }
    //--------------------------------------------------------------------------
    // 6. 创建初始值为 0 的 Fence，以及可反复用于 CPU 等待的自动重置事件。
    //--------------------------------------------------------------------------
    m_fenceEvent = CreateEventW(nullptr, false, false, nullptr);
    if (!m_fenceEvent) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
            "Creating the D3D12 fence event");
    }
    THROW_IF_FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
}

void RenderContext::BeginFrame() {
    auto b0 = std::chrono::high_resolution_clock::now();

    auto& frame = m_frameResources[m_frameIndex];
    m_lastWaitTriggered = 0;

    auto t0 = std::chrono::high_resolution_clock::now();
    // 只在当前要复用的帧槽仍有未完成提交时阻塞。
    if (frame.fenceValue > m_fence->GetCompletedValue()) {
        m_lastWaitTriggered = 1;
        WaitForFence(frame.fenceValue);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    m_lastWaitMs = std::chrono::duration<float, std::milli>(t1 - t0).count();

    // 上次使用此槽的 GPU 命令已经完成，可以回收临时上传空间和命令内存。
    frame.frameUpload.Reset();
    THROW_IF_FAILED(frame.cmdAllocator->Reset());
    THROW_IF_FAILED(frame.cmdList->Reset(frame.cmdAllocator.Get(), nullptr));

    auto b1 = std::chrono::high_resolution_clock::now();
    m_lastBeginMs = std::chrono::duration<float, std::milli>(b1 - b0).count();

}

void RenderContext::EndFrame() {
    auto t0 = std::chrono::high_resolution_clock::now();
    auto& frame = m_frameResources[m_frameIndex];
    THROW_IF_FAILED(frame.cmdList->Close());
    ID3D12CommandList* const lists[] = { frame.cmdList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
    auto t1 = std::chrono::high_resolution_clock::now();
    // 交换间隔为 0。统计 Present 调用占用的 CPU 时间。
    THROW_IF_FAILED(m_swapChain->Present(0, 0));
    auto t2 = std::chrono::high_resolution_clock::now();
    m_lastExecuteMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
    m_lastPresentMs = std::chrono::duration<float, std::milli>(t2 - t1).count();
    // 把 Fence 信号排在已提交工作之后，供当前帧槽下次复用时检查。
    m_currentFenceValue++;
    frame.fenceValue = m_currentFenceValue;
    THROW_IF_FAILED(m_commandQueue->Signal(m_fence.Get(), m_currentFenceValue));

    // CPU 轮换帧槽。当前实现也按这个索引选择后缓冲区。
    m_frameIndex = (m_frameIndex + 1) % m_maxFrameCount;
}

// 在 Direct 队列尾部放入 Fence 信号。等待它可以覆盖此前在该队列上提交的所有工作。
void RenderContext::WaitForGPU() {
    auto& frame = m_frameResources[m_frameIndex];
    m_currentFenceValue++;
    frame.fenceValue = m_currentFenceValue;
    THROW_IF_FAILED(m_commandQueue->Signal(m_fence.Get(), m_currentFenceValue));
    WaitForFence(m_currentFenceValue);
}

// Fence 达到目标值时触发事件，唤醒等待的 CPU 线程。
void RenderContext::WaitForFence(UINT64 fenceValue) {
    THROW_IF_FAILED(m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent));
    if (WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE) == WAIT_FAILED) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
            "Waiting for the D3D12 fence event");
    }
}



