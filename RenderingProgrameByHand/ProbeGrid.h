#pragma once
#include <d3d12.h>
#include <cstdint>
#include <DirectXMath.h>
#include "RenderTexture.h"

// 探针网格：CPU 描述 + GPU octahedral atlas 纹理
class ProbeGrid {
public:
    //新增一圈填充的保护边
    static constexpr uint32_t kTileSize = 8;
    static constexpr uint32_t kGutter = 1;
    static constexpr uint32_t kTileStride = kTileSize + 2 * kGutter; // 10
    // 距离矩 atlas：16×16 tile + 1px gutter = 18 stride
    static constexpr uint32_t kDistanceTileSize = 16;
    static constexpr uint32_t kDistanceTileStride = kDistanceTileSize + 2 * kGutter; // 18

    struct Desc {
        DirectX::XMFLOAT3 origin{ -2.2f, 0.3f, -2.2f };
        float             spacing = 0.63f;
        uint32_t          countX = 8;
        uint32_t          countY = 4;
        uint32_t          countZ = 8;
        uint32_t GetProbeCount() const { return countX * countY * countZ; }
    };

    void Init(ID3D12Device* device, GPUDescriptorAllocator& bindless, const Desc& desc);

    ID3D12Resource* GetResource() const noexcept { return m_atlas.GetResource(); }
    uint32_t        GetSRVIndex() const noexcept { return m_atlas.GetBindlessIndex(); }
    uint32_t        GetUAVIndex() const noexcept { return m_atlas.GetUAVBindlessIndex(); }
    const Desc& GetDesc()     const noexcept { return m_desc; }

    uint32_t GetAtlasWidth()  const { return m_desc.countX * kTileStride; }
    uint32_t GetAtlasHeight() const { return m_desc.countY * m_desc.countZ * kTileStride; }

    // 距离矩 atlas（RGBA16F：x = mean dist, y = mean dist²）
    ID3D12Resource* GetDistanceResource() const noexcept { return m_distanceAtlas.GetResource(); }
    uint32_t GetDistanceSRVIndex() const noexcept { return m_distanceAtlas.GetBindlessIndex(); }
    uint32_t GetDistanceUAVIndex() const noexcept { return m_distanceAtlas.GetUAVBindlessIndex(); }
    uint32_t GetDistanceAtlasWidth()  const { return m_desc.countX * kDistanceTileStride; }
    uint32_t GetDistanceAtlasHeight() const { return m_desc.countY * m_desc.countZ * kDistanceTileStride; }


private:
    RenderTexture m_atlas;
    RenderTexture m_distanceAtlas; //深度矩Texture
    Desc          m_desc{};    
};