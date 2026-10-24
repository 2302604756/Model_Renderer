#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <cstdint>
#include "CPUDescriptorAllocator.h"
#include "BindlessDescriptorHeap.h"

using Microsoft::WRL::ComPtr;

//离屏渲染目标：资源+RTV+bindless SRV + 当前状态
class RenderTexture {
public:
	void Init(ID3D12Device* device,
        CPUDescriptorAllocator& rtvAllocator,
        GPUDescriptorAllocator& bindlessAllocator,
        uint32_t width, uint32_t height,
        DXGI_FORMAT format,
        const float clearColor[4]);

    // 自动记录状态转换；状态相同则跳过
    void TransitionTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES target);

    D3D12_CPU_DESCRIPTOR_HANDLE GetRTV() const noexcept { return m_rtv; }
    uint32_t GetBindlessIndex() const noexcept { return m_bindlessIndex; }
    ID3D12Resource* GetResource() const noexcept { return m_resource.Get(); }
    D3D12_RESOURCE_STATES GetState() const noexcept { return m_state; }

private:
    ComPtr<ID3D12Resource>      m_resource;
    D3D12_CPU_DESCRIPTOR_HANDLE m_rtv{};
    uint32_t                    m_bindlessIndex = UINT32_MAX;
    D3D12_RESOURCE_STATES       m_state = D3D12_RESOURCE_STATE_COMMON;
};
