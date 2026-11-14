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
	static constexpr uint32_t kFrameCount = 3;

	void Build(ID3D12Device* device,
		GPUDescriptorAllocator& bindless,
		const MeshLibrary& meshLibrary,
		const Scene& scene);

	uint32_t       GetBindlessIndex(uint32_t frameIndex) const noexcept { return m_srvIndices[frameIndex % kFrameCount]; }
	ID3D12Resource* GetBuffer(uint32_t frameIndex)       const noexcept { return m_buffers[frameIndex % kFrameCount].Get(); }

	// 每帧写当前帧槽位
	void Update(uint32_t frameIndex,
		const MeshLibrary& meshLibrary,
		const Scene& scene);

private:
	Microsoft::WRL::ComPtr<ID3D12Resource>	m_buffers[kFrameCount];
	uint32_t								m_srvIndices[kFrameCount]{};
	uint32_t								m_capacity = 0;
};
