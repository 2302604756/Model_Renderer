#pragma once
#include <d3d12.h>
#include <cstdint>
#include <DirectXMath.h>
#include <vector>
#include "RenderTexture.h"
#include "ProbeVolume.h"


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
        // 包围盒向四周扩出约 1 格，保证墙面/地面都在网格内部，避免边界 clamp
        // y 从地板内部开始：最底层探针在地板里（validity 会判无效），
        // 不会出现在房间外空气里看到天空的探针
        // 每边留 0.15~0.25 的余量，保证墙面/天花板都在网格内部
        // y 起点放进地板立方体内部（地板 y ∈ [-0.10, 0]），
        // 避免最底层探针正好落在地板底面上
        DirectX::XMFLOAT3 origin{ -2.25f, 0.05f, -2.25f };
        float             spacing = 0.5f;
        uint32_t          countX = 10;
        uint32_t          countY = 5;
        uint32_t          countZ = 10;
        uint32_t GetProbeCount() const { return countX * countY * countZ; }
    };

    void Init(ID3D12Device* device, GPUDescriptorAllocator& bindless, const Desc& desc);

    ID3D12Resource* GetResource() const noexcept { return m_atlas.GetResource(); }
    uint32_t        GetSRVIndex() const noexcept { return m_atlas.GetBindlessIndex(); }
    uint32_t        GetUAVIndex() const noexcept { return m_atlas.GetUAVBindlessIndex(); }
    uint32_t        GetMaskSRVIndex() const noexcept { return m_maskSrvIndex; }
    const Desc& GetDesc()     const noexcept { return m_desc; }

    uint32_t GetAtlasWidth()  const { return m_desc.countX * kTileStride; }
    uint32_t GetAtlasHeight() const { return m_desc.countY * m_desc.countZ * kTileStride; }

    // 距离矩 atlas（RGBA16F：x = mean dist, y = mean dist²）
    ID3D12Resource* GetDistanceResource() const noexcept { return m_distanceAtlas.GetResource(); }
    ID3D12Resource* GetProbeDataResource() const noexcept { return m_probeDataBuffer.Get(); }
    ID3D12Resource* GetFilteredResource() const noexcept { return m_filteredAtlas.GetResource(); }

    uint32_t GetDistanceSRVIndex() const noexcept { return m_distanceAtlas.GetBindlessIndex(); }
    uint32_t GetDistanceUAVIndex() const noexcept { return m_distanceAtlas.GetUAVBindlessIndex(); }
    uint32_t GetDistanceAtlasWidth()  const { return m_desc.countX * kDistanceTileStride; }
    uint32_t GetDistanceAtlasHeight() const { return m_desc.countY * m_desc.countZ * kDistanceTileStride; }

    uint32_t GetProbeDataSRVIndex() const noexcept { return m_probeDataSrvIndex; }
    uint32_t GetFilteredSRVIndex() const noexcept { return m_filteredAtlas.GetBindlessIndex(); }
    uint32_t GetProbeDataUAVIndex() const noexcept { return m_probeDataUavIndex; }
    uint32_t GetFilteredUAVIndex() const noexcept { return m_filteredAtlas.GetUAVBindlessIndex(); }

    void BuildMasks(ID3D12Device* device,
        GPUDescriptorAllocator& bindless,
        const std::vector<ProbeVolume>& volumes);




private:
    RenderTexture m_atlas;
    RenderTexture m_filteredAtlas;
    RenderTexture m_distanceAtlas; //深度矩Texture
    Desc          m_desc{};    

    Microsoft::WRL::ComPtr<ID3D12Resource> m_maskBuffer;
    uint32_t m_maskSrvIndex = UINT32_MAX;
    std::vector<uint32_t> m_probeMasks;

    // offest buffer
    Microsoft::WRL::ComPtr<ID3D12Resource> m_probeDataBuffer;
    uint32_t m_probeDataSrvIndex = UINT32_MAX;
    uint32_t m_probeDataUavIndex = UINT32_MAX;
};