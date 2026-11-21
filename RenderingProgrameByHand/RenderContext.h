#pragma once
#include "d3d12lite.h"
#include "UploadHeap.h"
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>	

// 一个帧槽持有独立的命令分配器、命令列表和上传堆。最近一次提交对应的 Fence
// 完成后，CPU 才能重置分配器和上传堆，避免覆盖 GPU 仍在使用的数据。
struct FrameResource {
	ComPtr<ID3D12CommandAllocator>			cmdAllocator;
	ComPtr<ID3D12GraphicsCommandList>		cmdList;
	UploadHeap								frameUpload;
	UINT64									fenceValue = 0;
};

// Renderer 持有此上下文。它负责设备、交换链、Direct 队列和帧槽同步。
// Renderer 在释放渲染资源前显式等待队列完成，随后由此类释放事件句柄和 COM 引用。
class RenderContext {
public:
	// 创建固定数量的帧槽。该数量也用于初始化交换链缓冲区。
	RenderContext(uint32_t frameCount = 3)
		: m_maxFrameCount(frameCount)
		, m_frameResources(frameCount)
		, m_frameIndex(0)
	{
		s_instance = this;
	}

	~RenderContext() noexcept;
	RenderContext(const RenderContext&) = delete;
	RenderContext& operator=(const RenderContext&) = delete;

	// 为外部 HWND 创建设备、队列和交换链，准备帧槽和 Fence 等待事件。
	void Init(HWND hwnd, uint32_t width, uint32_t height);
	// 等待当前帧槽上次提交，再重置该槽，开始新一轮命令录制。
	void BeginFrame();
	// 提交当前帧、Present 并记录 Fence，最后轮换到下一帧槽。
	void EndFrame();
	// 等待此前提交到 Direct 队列的工作完成，用于初始化上传之后和退出时。
	void WaitForGPU();


	// 为描述符分配器等现有工具保留的借用入口。调用方需在上下文初始化后、
	// 析构前使用它，实际生命周期由 Renderer 管理。
	static RenderContext& Get() { return *s_instance; }

	// 这些指针和引用借自上下文。队列也可用于提交启动时的上传及加速结构构建。
	ID3D12Device* GetDevice() const { return m_device.Get(); };
	IDXGISwapChain* GetSwapChian() const { return m_swapChain.Get(); }
	FrameResource& GetFrame() { return m_frameResources[m_frameIndex]; }
	ID3D12CommandQueue* GetCommandQueue() { return m_commandQueue.Get(); }
	// 软件维护的帧槽编号。现有 Renderer 同时用它索引后缓冲区 RTV。
	uint32_t GetFrameIndex() { return m_frameIndex; }
	uint32_t GetFrameCount() { return m_maxFrameCount; }

	// 当前 D3D12 设备实际选中的适配器名字，用于窗口标题/调试显示
	const wchar_t* GetAdapterName() const noexcept { return m_adapterName.c_str(); }


	// Renderer 以 dt 累加的运行时间，单位为秒，随帧常量传入着色器。
	float time = 0.0f;

	// 下列数据为 CPU 侧计时。Execute 包含关闭与提交命令列表，Present 单独记录调用耗时。
	// Wait 记录 Fence 检查及可能的阻塞，Begin 还包括上传堆和命令对象的重置。
	float GetLastExecuteMs() const noexcept { return m_lastExecuteMs; }
	float GetLastPresentMs() const noexcept { return m_lastPresentMs; }
	float GetLastWaitMs() const noexcept { return m_lastWaitMs; }
	float GetLastBeginMs() const noexcept { return m_lastBeginMs; }
	// 最近一次 BeginFrame 进入 Fence 等待分支时为 1，否则为 0。
	uint32_t GetLastWaitTriggered() const noexcept { return m_lastWaitTriggered; }


private:
	// 为目标 Fence 值注册同一个自动重置事件，然后阻塞调用线程。
	void WaitForFence(UINT64 fenceValue);

	// 构造时登记的非拥有指针，由同一实例在析构时注销。
	inline static RenderContext* s_instance = nullptr;

	uint32_t				   m_frameIndex = 0;
	// 固定大小的帧槽环，EndFrame 推进 m_frameIndex。
	uint32_t				   m_maxFrameCount;
	std::vector<FrameResource> m_frameResources;

	// Direct 队列、Fence、CPU 等待事件和已分配的最后一个 Fence 值。
	ComPtr<ID3D12CommandQueue> m_commandQueue;
	ComPtr<ID3D12Fence>		   m_fence;
	HANDLE					   m_fenceEvent = nullptr;
	uint32_t				   m_currentFenceValue = 0;

	// 核心 DXGI 和 D3D12 对象由 ComPtr 管理。
	ComPtr<IDXGIFactory4>      m_factory;
	ComPtr<ID3D12Device>       m_device;
	ComPtr<IDXGISwapChain3>    m_swapChain;
	std::wstring               m_adapterName; // 实际选中适配器名称

	// 最近一帧的 CPU 计时，耗时以毫秒记录。
	float m_lastExecuteMs = 0.0f;
	float m_lastPresentMs = 0.0f;
	float m_lastWaitMs = 0.0f;
	float m_lastBeginMs = 0.0f;
	uint32_t m_lastWaitTriggered = 0;

};

// 对失败的 HRESULT 抛出异常，保留源码位置和返回值。
#define THROW_IF_FAILED(hr)                                                     \
    do {                                                                        \
        HRESULT _hr = (hr);                                                     \
        if (FAILED(_hr)) {                                                      \
            throw std::runtime_error(std::string("D3D12 Error in ") + __FILE__  \
                + " at line " + std::to_string(__LINE__)                        \
                + " Code: " + std::to_string(_hr));                             \
        }                                                                       \
    } while (0)