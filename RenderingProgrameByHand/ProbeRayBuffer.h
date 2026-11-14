#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <cstdint>
#include "BindlessDescriptorHeap.h"

// probeCount × rayCount 的 ray 数据 buffer
class ProbeRayBuffer {
public:
    static constexpr uint32_t kRayCount = 256;     // 每个探针的射线数
    static constexpr uint32_t kFixedRayCount = 32; // 用于对探针进行offset偏置的raycounts

    void Init(ID3D12Device* device, GPUDescriptorAllocator& bindless, uint32_t totalRays);

    ID3D12Resource* GetResource() const noexcept { return m_buffer.Get(); }
    uint32_t GetUAVIndex() const noexcept { return m_uavIndex; }
    uint32_t GetSRVIndex() const noexcept { return m_srvIndex; }
    uint32_t GetTotalRays() const noexcept { return m_totalRays; }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_buffer;
    uint32_t m_uavIndex = UINT32_MAX;
    uint32_t m_srvIndex = UINT32_MAX;
    uint32_t m_totalRays = 0;
};