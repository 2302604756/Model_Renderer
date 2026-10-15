#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <cstdint>
#include <cassert>
#include <stdexcept>

//bindless系统就需要 GPU handle
//让资源去持有 GPUDescriptorHandle

using Microsoft::WRL::ComPtr;

struct GPUDescriptorHandle {
	D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = { 0 };
	D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = { 0 };
	uint32_t					index = UINT32_MAX;

	bool IsValid() const { return index != UINT32_MAX; }
};

class GPUDescriptorAllocator {
public:
	GPUDescriptorAllocator(uint32_t maxCount=10000) 
	:m_capacity(maxCount){

	}

	//禁用拷贝
	GPUDescriptorAllocator(const GPUDescriptorAllocator&) = delete;
	GPUDescriptorAllocator& operator=(const GPUDescriptorAllocator&) = delete;

	void Init();
	GPUDescriptorHandle Allocate();

	// 供 RenderContext / CommandList 绑定堆时使用
	ID3D12DescriptorHeap* GetHeap() const noexcept {
		return m_heap.Get();
	}

private:
	
	ComPtr<ID3D12DescriptorHeap> m_heap;

	std::vector<uint32_t>		 m_freelists;

	uint32_t				     m_capacity = 0;
	uint32_t					 m_descriptorSize = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE  m_cpuHandle = { 0 };
	D3D12_GPU_DESCRIPTOR_HANDLE  m_gpuHandle = { 0 };
};