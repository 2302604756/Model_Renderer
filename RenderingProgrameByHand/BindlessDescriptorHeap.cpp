#include "BindlessDescriptorHeap.h"
#include "RenderContext.h"
#include "d3d12lite.h"

void GPUDescriptorAllocator::Init() {
	ID3D12Device* device = RenderContext::Get().GetDevice();
	m_heap = d3d12lite::CreateSRVHeap(m_capacity);

	//初始化freelist
	m_freelists.reserve(m_capacity);
	for (int i = 0; i < m_capacity; i++) {
		m_freelists.push_back(m_capacity - i - 1);
	}
	
	m_cpuHandle = m_heap->GetCPUDescriptorHandleForHeapStart();
	m_gpuHandle = m_heap->GetGPUDescriptorHandleForHeapStart();
	m_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

}

GPUDescriptorHandle GPUDescriptorAllocator::Allocate() {
	ID3D12Device* device = RenderContext::Get().GetDevice();
	assert(!m_freelists.empty() && "Bindeless Descriptor Pool Exhausted!");
	uint32_t slot = m_freelists.back();
	//计算实际handle
	D3D12_CPU_DESCRIPTOR_HANDLE  cpuHandle = { m_cpuHandle.ptr + slot * m_descriptorSize };
	D3D12_GPU_DESCRIPTOR_HANDLE  gpuHandle = { m_gpuHandle.ptr + slot * m_descriptorSize };

	m_freelists.pop_back();
	return { cpuHandle,gpuHandle,slot };
}
