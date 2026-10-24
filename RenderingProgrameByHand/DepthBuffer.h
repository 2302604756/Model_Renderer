#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <cstdint>
#include "CPUDescriptorAllocator.h"

using Microsoft::WRL::ComPtr;

class DepthBuffer {
public:
    static constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_D32_FLOAT;

    void Init(ID3D12Device* device,
        CPUDescriptorAllocator& dsvAllocator,
        uint32_t width, uint32_t height);

    void Clear(ID3D12GraphicsCommandList* cmdList) const noexcept;

    inline D3D12_CPU_DESCRIPTOR_HANDLE GetDSV() const noexcept { return m_dsv; }

    ID3D12Resource* GetResource() const noexcept { return m_resource.Get(); }

private:
    ComPtr<ID3D12Resource>      m_resource;
    D3D12_CPU_DESCRIPTOR_HANDLE m_dsv{};
    uint32_t                    m_width = 0;
    uint32_t                    m_height = 0;
};