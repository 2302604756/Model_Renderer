#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <cstdint>
#include <cassert>


//工业级标准做法（Sub-allocation）：直接创建一个巨大的 ID3D12Resource 线性 Buffer（如 64 MB），
// 在初始化时调用一次 Map 永久保持映射（Persistently Mapped）。
// 后续无论是上传贴图还是传顶点/常量，只需移动内部的字节游标（Bump Pointer）返回切片地址，开销为 0，
// 且对齐粒度只需 512 字节（纹理上传要求）或 256 字节（常量缓冲要求）。

using Microsoft::WRL::ComPtr;

struct UploadBlock{
	ID3D12Resource*           buffer = nullptr;			//和TransientHeap的GpuMemoreyBlock持有Heap指针一样
	uint8_t*				  cpuAddress = nullptr;
	D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0;
	uint64_t                  offset = 0;
	uint64_t                  size = 0;
};

class UploadHeap {
public:
	UploadHeap() = default;
	~UploadHeap();

	UploadHeap(const UploadHeap&) = delete;
	UploadHeap& operator=(const UploadHeap&) = delete;

	void Init(uint64_t capacity = 64 * 1024 * 1024);

	// 分配一段对齐的上传内存
	// 纹理上传需传入 D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT (512)
	// 常量缓冲需传入 D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT (256)
	UploadBlock Allocate(uint64_t sizeInBytes, uint64_t alignment = 512);

	// 当 GPU 执行完拷贝后，重置游标以复用整块上传内存
	void Reset() noexcept { m_currentOffset = 0; }

private:
	ComPtr<ID3D12Resource>    m_uploadBuffer;
	uint8_t*				  m_cpuBase			= 0;
	D3D12_GPU_VIRTUAL_ADDRESS m_gpuBase			= 0;
	uint64_t				  m_capacity		= 0;
	uint64_t				  m_currentOffset	= 0;

};

