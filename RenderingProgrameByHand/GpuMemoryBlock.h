#pragma once
#include <d3d12.h>
#include <cstdint>

struct GpuMemoryBlock {
	ID3D12Heap* heap   = nullptr;
	uint64_t    offset = 0;
	uint64_t    size = 0;

	bool IsValid() const noexcept { return heap != nullptr; }
};