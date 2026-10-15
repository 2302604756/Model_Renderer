#include "RenderContext.h"
#include <dxgi1_6.h>
#include <stdexcept>
#include <string>
#include "d3d12lite.h"

using namespace d3d12lite;

void RenderContext::Init(HWND hwnd, uint32_t width, uint32_t height) {
    // -------------------------------------------------------------------------
    // 1. 启用 D3D12 Debug Layer (仅在 Debug 模式下)
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

    // 枚举高算力硬件显卡（跳过软件适配器）
    ComPtr<IDXGIAdapter1> adapter;
    for (uint32_t i = 0; m_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }

        // 优先锁定 Feature Level 12.0
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)))) {
            break;
        }
    }

    if (!m_device) {
        throw std::runtime_error("Failed to find a compatible Direct3D 12 physical device.");
    }
    // -------------------------------------------------------------------------
    // 3. 创建主 Direct 命令队列 (Command Queue)
    // -------------------------------------------------------------------------
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    THROW_IF_FAILED(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));
    
    // -------------------------------------------------------------------------
    // 4. 创建交换链 (Swap Chain - FLIP_DISCARD 现代模型)
    // -------------------------------------------------------------------------
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = m_maxFrameCount; // 缓冲链深度与在途帧数对齐
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

    // 拦截 DXGI 默认的 Alt+Enter 全屏切换行为
    THROW_IF_FAILED(m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));
    THROW_IF_FAILED(swapChain1.As(&m_swapChain));

    //-------------------------------------------------------------------------
    // 5. 初始化环形缓冲槽位 (Frame Resources)
    // -------------------------------------------------------------------------
    for (int i = 0; i < m_maxFrameCount; i++) {
        THROW_IF_FAILED(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_frameResources[i].cmdAllocator)
        ));
        // 为该槽位创建配套的 CommandList
        THROW_IF_FAILED(m_device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_frameResources[i].cmdAllocator.Get(),
            nullptr, // 初始 PSO 置空
            IID_PPV_ARGS(&m_frameResources[i].cmdList)
        ));
        // CreateCommandList 创建后处于录制状态，初始化完毕必须关闭
        THROW_IF_FAILED(m_frameResources[i].cmdList->Close());

        m_frameResources[i].fenceValue = 0;
    }
    //--------------------------------------------------------------------------
    //6.fence event初始化
    //--------------------------------------------------------------------------
    m_fenceEvent = CreateEventW(nullptr, false, false, nullptr);
    THROW_IF_FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
}

void RenderContext::BeginFrame() {
   auto &frame = m_frameResources[m_frameIndex];
   if (frame.fenceValue > m_fence->GetCompletedValue()) {
       THROW_IF_FAILED(m_fence->SetEventOnCompletion(frame.fenceValue, m_fenceEvent));
       WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
   }
   THROW_IF_FAILED(frame.cmdAllocator->Reset());
   THROW_IF_FAILED(frame.cmdList->Reset(frame.cmdAllocator.Get(), nullptr));
}

void RenderContext::EndFrame() {
    auto &frame = m_frameResources[m_frameIndex];
    THROW_IF_FAILED(frame.cmdList->Close());
    ID3D12CommandList* const lists[] = {frame.cmdList.Get()};
    m_commandQueue->ExecuteCommandLists(1,lists);
    THROW_IF_FAILED(m_swapChain->Present(1, 0));
    // 4. 发出 GPU 时间线推进信号
    m_currentFenceValue++;
    frame.fenceValue = m_currentFenceValue;
    THROW_IF_FAILED(m_commandQueue->Signal(m_fence.Get(), m_currentFenceValue));

    // 5. 推进到环形缓冲的下一个槽位
    m_frameIndex = (m_frameIndex + 1) % m_maxFrameCount;
}

void RenderContext::WaitForGPU() {
    auto &frame = m_frameResources[m_frameIndex];
    m_currentFenceValue++;
    frame.fenceValue = m_currentFenceValue;
    THROW_IF_FAILED(m_commandQueue->Signal(m_fence.Get(), m_currentFenceValue));
    THROW_IF_FAILED(m_fence->SetEventOnCompletion(m_currentFenceValue, m_fenceEvent));
    WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
}



