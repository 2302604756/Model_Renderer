#pragma once
#include "d3d12lite.h"
#include "UploadHeap.h"
#include <cstdint>
#include <vector>
#include <chrono>	

struct FrameResource {
	ComPtr<ID3D12CommandAllocator>			cmdAllocator;
	ComPtr<ID3D12GraphicsCommandList>		cmdList;
	UploadHeap								frameUpload;
	UINT64									fenceValue = 0;
};

//用一个简易的Context类维护全局同步变量以及
class RenderContext {
public:
	RenderContext(uint32_t frameCount = 3)
		: m_maxFrameCount(frameCount)
		, m_frameResources(frameCount)
		, m_frameIndex(0)
	{
		s_instance = this;
	}

	//uint32_t来自cpp标准，可移植性强
	void Init(HWND hwnd,uint32_t width,uint32_t height);
	void BeginFrame();
	void EndFrame();
	void WaitForGPU();


	//获取静态实例
	static RenderContext& Get() { return *s_instance; }

	ID3D12Device* GetDevice() const { return m_device.Get(); };
	IDXGISwapChain* GetSwapChian() const { return m_swapChain.Get(); }
	FrameResource&  GetFrame() { return m_frameResources[m_frameIndex];}
	ID3D12CommandQueue* GetCommandQueue() { return m_commandQueue.Get(); }
	uint32_t GetFrameIndex() { return m_frameIndex; }
	uint32_t GetFrameCount() { return m_maxFrameCount; }


	//用于时间实验
	float time = 0.0f;

	//用于监控EndFrame内用时
	float GetLastExecuteMs() const noexcept { return m_lastExecuteMs; }
	float GetLastPresentMs() const noexcept { return m_lastPresentMs; }
	float GetLastWaitMs() const noexcept { return m_lastWaitMs; }
	float GetLastBeginMs() const noexcept { return m_lastBeginMs; }
	uint32_t GetLastWaitTriggered() const noexcept { return m_lastWaitTriggered; }
	

private:
	//静态实例
	inline static RenderContext*	 s_instance = nullptr;
	
	uint32_t				   m_frameIndex = 0;
	//环形缓冲资列表
	uint32_t				   m_maxFrameCount;
	std::vector<FrameResource> m_frameResources;

	//全局命令执行与同步
	ComPtr<ID3D12CommandQueue> m_commandQueue;
	ComPtr<ID3D12Fence>		   m_fence;
	HANDLE					   m_fenceEvent = nullptr;
	uint32_t				   m_currentFenceValue = 0;

	//核心设备与交换链
	ComPtr<IDXGIFactory4>      m_factory;
	ComPtr<ID3D12Device>       m_device;
	ComPtr<IDXGISwapChain3>    m_swapChain;

	//用于监控用时的变量
	float m_lastExecuteMs = 0.0f;
	float m_lastPresentMs = 0.0f;
	float m_lastWaitMs = 0.0f;
	float m_lastBeginMs = 0.0f;
	uint32_t m_lastWaitTriggered = 0;
	
};

//自定义error宏
#define THROW_IF_FAILED(hr)                                                     \
    do {                                                                        \
        HRESULT _hr = (hr);                                                     \
        if (FAILED(_hr)) {                                                      \
            throw std::runtime_error(std::string("D3D12 Error in ") + __FILE__  \
                + " at line " + std::to_string(__LINE__)                        \
                + " Code: " + std::to_string(_hr));                             \
        }                                                                       \
    } while (0)