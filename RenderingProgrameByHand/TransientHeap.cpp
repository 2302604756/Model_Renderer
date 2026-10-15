#include "TransientHeap.h"
#include "RenderContext.h"

void TransientHeap::Init(uint64_t capacity) {
	ID3D12Device* device = RenderContext::Get().GetDevice();
	m_capacity = capacity;

	D3D12_HEAP_PROPERTIES prop = {};
	prop.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_HEAP_DESC desc;
	desc.SizeInBytes = capacity;
	desc.Flags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;//允许存放各类buffer 和 texture
	desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;//64kb对齐
	desc.Properties = prop;

	THROW_IF_FAILED(device->CreateHeap(&desc, IID_PPV_ARGS(&m_heap)));

	m_currentOffset = 0;
}

GpuMemoryBlock TransientHeap::Allocate(const D3D12_RESOURCE_DESC& desc) {
	ID3D12Device* device = RenderContext::Get().GetDevice();
	// 1. 获取驱动要求该资源必须遵守的真实显存大小与对齐约束
	D3D12_RESOURCE_ALLOCATION_INFO allocInfo = device->GetResourceAllocationInfo(0, 1, &desc);
	// 2. 向上计算严格对齐后的起始偏移
	uint64_t alignment = allocInfo.Alignment;
	uint64_t alignedOffset = (m_currentOffset + alignment - 1) & ~(alignment - 1);

	assert(alignedOffset + allocInfo.SizeInBytes <= m_capacity && "Transient VRAM Heap Out of Memory!");

	// 3. 推进内部线性游标
	m_currentOffset = alignedOffset + allocInfo.SizeInBytes;

	return { m_heap.Get(), alignedOffset, allocInfo.SizeInBytes };
}

void TransientHeap::Reset() noexcept {
	m_currentOffset = 0;
}