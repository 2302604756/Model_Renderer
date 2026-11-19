#include "ProbeGrid.h"
#include "RenderContext.h"
#include <cstring>

void ProbeGrid::Init(ID3D12Device* device, GPUDescriptorAllocator& bindless, const Desc& desc)
{
    m_desc = desc;
    m_atlas.InitUAV(device, bindless,
        GetAtlasWidth(), GetAtlasHeight(),
        DXGI_FORMAT_R11G11B10_FLOAT);   // 初始状态 UNORDERED_ACCESS

    // 距离矩 atlas：RGBA16F，x=mean dist，y=mean dist²
    m_distanceAtlas.InitUAV(device, bindless,
        GetDistanceAtlasWidth(), GetDistanceAtlasHeight(),
        DXGI_FORMAT_R16G16B16A16_FLOAT);

    m_filteredAtlas.InitUAV(
        device,
        bindless,
        GetAtlasWidth(),
        GetAtlasHeight(),
        DXGI_FORMAT_R11G11B10_FLOAT);

    const uint32_t probeCount = m_desc.GetProbeCount();
    const uint64_t bytes =
        static_cast<uint64_t>(probeCount) * sizeof(DirectX::XMFLOAT4);

    // ----------- Offest Resource Init --------------
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = bytes;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc = { 1, 0 };
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_GPU_UPLOAD;

    THROW_IF_FAILED(device->CreateCommittedResource(
        &props, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&m_probeDataBuffer)));

    // 初始 offset = 0
    void* mapped = nullptr;
    THROW_IF_FAILED(m_probeDataBuffer->Map(0, nullptr, &mapped));
    std::memset(mapped, 0, bytes);
    m_probeDataBuffer->Unmap(0, nullptr);

    GPUDescriptorHandle uav = bindless.Allocate();
    m_probeDataUavIndex = uav.index;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = probeCount;
    uavDesc.Buffer.StructureByteStride = sizeof(DirectX::XMFLOAT4);
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

    device->CreateUnorderedAccessView(
        m_probeDataBuffer.Get(), nullptr, &uavDesc, uav.cpuHandle);
}

void ProbeGrid::BuildMasks(ID3D12Device* device,
    GPUDescriptorAllocator& bindless,
    const std::vector<ProbeVolume>& volumes)
{
    const uint32_t count = m_desc.GetProbeCount();
    m_probeMasks.assign(count, 0u);

    for (uint32_t z = 0; z < m_desc.countZ; ++z)
        for (uint32_t y = 0; y < m_desc.countY; ++y)
            for (uint32_t x = 0; x < m_desc.countX; ++x)
            {
                DirectX::XMFLOAT3 p{
                    m_desc.origin.x + x * m_desc.spacing,
                    m_desc.origin.y + y * m_desc.spacing,
                    m_desc.origin.z + z * m_desc.spacing
                };

                uint32_t mask = 0;
                for (const ProbeVolume& v : volumes)
                {
                    if (p.x >= v.boundsMin.x && p.x <= v.boundsMax.x &&
                        p.y >= v.boundsMin.y && p.y <= v.boundsMax.y &&
                        p.z >= v.boundsMin.z && p.z <= v.boundsMax.z)
                    {
                        mask |= v.mask;
                    }
                }

                uint32_t index = (z * m_desc.countY + y) * m_desc.countX + x;
                m_probeMasks[index] = mask;
            }

    // 没有 volume 时保留旧行为：所有探针 mask=1
    if (volumes.empty())
        m_probeMasks.assign(count, 1u);

    const uint64_t bytes = static_cast<uint64_t>(count) * sizeof(uint32_t);

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc = { 1, 0 };
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_GPU_UPLOAD;

    THROW_IF_FAILED(device->CreateCommittedResource(
        &props, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_maskBuffer)));

    void* mapped = nullptr;
    THROW_IF_FAILED(m_maskBuffer->Map(0, nullptr, &mapped));
    std::memcpy(mapped, m_probeMasks.data(), bytes);
    m_maskBuffer->Unmap(0, nullptr);

    GPUDescriptorHandle handle = bindless.Allocate();
    m_maskSrvIndex = handle.index;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = 0;
    srv.Buffer.NumElements = count;
    srv.Buffer.StructureByteStride = sizeof(uint32_t);

    device->CreateShaderResourceView(
        m_maskBuffer.Get(), &srv, handle.cpuHandle);
}