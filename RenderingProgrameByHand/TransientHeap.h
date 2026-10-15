#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <cstdint>
#include <cassert>
#include <stdexcept>
#include "GpuMemoryBlock.h"

//resource本身仅持有Block

using Microsoft::WRL::ComPtr;

class TransientHeap {
public:
	TransientHeap() = default;
	~TransientHeap() = default;

	TransientHeap& operator=(const TransientHeap&) = delete;
	TransientHeap(const TransientHeap& heap) = delete;

	void Init(uint64_t capacity = 256 * 1024 * 1024 );
	//分配方法
	GpuMemoryBlock Allocate(const D3D12_RESOURCE_DESC& desc);
	// 显存复用核心：当 Pass 结束或帧图执行完后，一键重置游标实现物理地址别名覆写
	void Reset() noexcept;
	// 获取当前游标，用于记录资源生命周期区间
	uint64_t GetCurrentOffset() const noexcept { return m_currentOffset; }
	void     SetCurrentOffset(uint64_t offset) noexcept { m_currentOffset = offset; }

	ID3D12Heap* GetHeap() const noexcept { return m_heap.Get(); }
	
private:
	ComPtr<ID3D12Heap>   m_heap;
	uint64_t             m_capacity = 0;
	uint64_t             m_currentOffset = 0;//指定空闲offset
};
