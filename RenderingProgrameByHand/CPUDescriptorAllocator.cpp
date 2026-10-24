#include "CPUDescriptorAllocator.h"
#include "RenderContext.h"
#include "d3d12lite.h"

void CPUDescriptorAllocator::Init() {
	ID3D12Device* device = RenderContext::Get().GetDevice();
	IDXGISwapChain* swapchain = RenderContext::Get().GetSwapChian();
	uint32_t frameCount = RenderContext::Get().GetFrameCount();

	//仅建堆，不写入view 后续交给allocate
	m_rtvHeap = d3d12lite::CreateRTVHeap(m_capacity);
	m_dsvHeap = d3d12lite::CreateDSVHeap(m_capacity);

	//freelist初始化
	m_rtvFreeLists.reserve(m_capacity);
	m_dsvFreeLists.reserve(m_capacity);

	for (int i = 0; i < m_capacity; i++) {
		m_rtvFreeLists.push_back(m_capacity - i - 1);
	}
	for (int i = 0; i < m_capacity; i++) {
		m_dsvFreeLists.push_back(m_capacity - i - 1);
	}
	m_baseRTVHanle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	m_baseDSVHanle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

	m_rtvResource.resize(frameCount);
	m_rtvBackBuffer.resize(frameCount);

	for (int i = 0; i < frameCount; i++) {
		CPUDescriptorHandle handle = RTVAllocate();
		m_rtvBackBuffer[i] = handle;
		swapchain->GetBuffer(i, IID_PPV_ARGS(&m_rtvResource[i]));
		device->CreateRenderTargetView(m_rtvResource[i].Get(), nullptr, handle.cpuHandle);

	}
}

CPUDescriptorHandle CPUDescriptorAllocator::RTVAllocate() {
	
	ID3D12Device* device = RenderContext::Get().GetDevice();
	//使用断言来检测slot是否分配完全 仅在debug生效
	assert(!m_rtvFreeLists.empty() && "CPU Descriptor Pool Exhausted!");
	uint32_t slot = m_rtvFreeLists.back();
	uint32_t size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	//计算实际handle base+offset
	D3D12_CPU_DESCRIPTOR_HANDLE handle = { m_baseRTVHanle.ptr + slot * size };
	m_rtvFreeLists.pop_back();
	return{ handle,slot };
}

CPUDescriptorHandle CPUDescriptorAllocator::DSVAllocate() {
	ID3D12Device* device = RenderContext::Get().GetDevice();
	//使用断言来检测slot是否分配完全 仅在debug生效
	assert(!m_dsvFreeLists.empty() && "CPU Descriptor Pool Exhausted!");
	uint32_t slot = m_dsvFreeLists.back();
	uint32_t size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	//计算实际handle base+offset
	D3D12_CPU_DESCRIPTOR_HANDLE handle = { m_baseDSVHanle.ptr + slot * size };
	m_dsvFreeLists.pop_back();
	return{ handle,slot };
}