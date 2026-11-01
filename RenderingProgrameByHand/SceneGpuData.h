#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <cstdint>
#include "BindlessDescriptorHeap.h"

class MeshLibrary;
class Scene;

// 把 CPU 场景转换成 GPU 侧实例数据（GPUObjectData buffer + bindless SRV）
class SceneGpuData {
public:
	void Build(ID3D12Device* device,
		GPUDescriptorAllocator& bindless,
		const MeshLibrary& meshLibrary,
		const Scene& scene);

	uint32_t       GetBindlessIndex() const noexcept { return m_bindlessIndex; }
	ID3D12Resource* GetBuffer()       const noexcept { return m_buffer.Get(); }

private:
	Microsoft::WRL::ComPtr<ID3D12Resource> m_buffer;
	uint32_t                               m_bindlessIndex = UINT32_MAX;
};
