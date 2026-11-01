#include "ProbeGrid.h"

void ProbeGrid::Init(ID3D12Device* device, GPUDescriptorAllocator& bindless, const Desc& desc)
{
    m_desc = desc;
    m_atlas.InitUAV(device, bindless,
        GetAtlasWidth(), GetAtlasHeight(),
        DXGI_FORMAT_R11G11B10_FLOAT);   // 初始状态 UNORDERED_ACCESS

    // 距离矩 atlas：RGBA16F，x=mean dist，y=mean dist²
    m_distanceAtlas.InitUAV(device, bindless,
        GetDistanceAtlasWidth(), GetDistanceAtlasHeight(),
        DXGI_FORMAT_R16G16_FLOAT);
}