#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <cstdint>
#include <cassert>
#include <stdexcept>

//需要什么
//在allocate时我们需要1.basehandle 2.资源地址 3.size  在create时要拿size*index算出offset 最后返回slot
//为默认纹理，描述符保留静态槽位，如swapchain的backbuffer 在初始化时变create rtv
//Allocate仅仅只分配槽位

//分配好了槽位以后要干什么呢，我们需要用槽位去计算handle ，然后去往对应槽位写view 
using Microsoft::WRL::ComPtr;

struct CPUDescriptorHandle {
	D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle{ 0 };
	uint32_t                    index = UINT32_MAX;

	bool IsValid() const { return index != UINT32_MAX;}
};

class CPUDescriptorAllocator {
public:
	CPUDescriptorAllocator(uint32_t maxCount = 500)
		: m_capacity(maxCount) {

	}
	
	// 禁用拷贝
	CPUDescriptorAllocator(const CPUDescriptorAllocator&) = delete;
	CPUDescriptorAllocator& operator=(const CPUDescriptorAllocator&) = delete;

	void Init();
	CPUDescriptorHandle RTVAllocate();

	// 供 RenderContext / CommandList 绑定堆时使用
	ID3D12DescriptorHeap*   GetBaseRTVHeap()  const noexcept { return m_rtvHeap.Get(); }
	ID3D12DescriptorHeap*   GetBaseDSVHeap()  const noexcept { return m_dsvHeap.Get(); }

	//返回backbuffer
	ID3D12Resource* GetBackBufferResource(uint32_t frameIndex) const noexcept { return m_rtvResource[frameIndex].Get(); }
	D3D12_CPU_DESCRIPTOR_HANDLE GetBackBufferRTV(uint32_t frameIndex)const noexcept { return m_rtvBackBuffer[frameIndex].cpuHandle; }
private:
	//backbuffer写入
	std::vector<ComPtr<ID3D12Resource>>	m_rtvResource;
	std::vector<CPUDescriptorHandle>	m_rtvBackBuffer;	//D3D12_CPU_DESCRIPTOR_HANDLE 都不是接口也不是COM对象怎么会去用ComPtr呢

	ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
	ComPtr<ID3D12DescriptorHeap> m_dsvHeap;

	std::vector<uint32_t>        m_rtvFreeLists;
	std::vector<uint32_t>		 m_dsvFreeLists;

	uint32_t                     m_capacity = 0;
	uint32_t					 m_descriptorSize = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE  m_baseRTVHanle = { 0 };
	D3D12_CPU_DESCRIPTOR_HANDLE  m_baseDSVHanle = { 0 };
};