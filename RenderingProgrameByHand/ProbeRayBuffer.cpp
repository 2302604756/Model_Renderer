#include "ProbeRayBuffer.h"
#include "RenderContext.h" // THROW_IF_FAILED

void ProbeRayBuffer::Init(ID3D12Device* device, GPUDescriptorAllocator& bindless, uint32_t totalRays)
{
    m_totalRays = totalRays;

    // StructuredBuffer<ProbeRay>：每条 64 字节
    const uint64_t bytes = static_cast<uint64_t>(totalRays) * 64;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (bytes + 255ull) & ~255ull;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc = { 1, 0 };
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;

    THROW_IF_FAILED(device->CreateCommittedResource(
        &props, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&m_buffer)));

    // UAV：ray pass / shade pass 写
    GPUDescriptorHandle uav = bindless.Allocate();
    m_uavIndex = uav.index;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = totalRays;
    uavDesc.Buffer.StructureByteStride = 64;
    device->CreateUnorderedAccessView(m_buffer.Get(), nullptr, &uavDesc, uav.cpuHandle);

    // SRV：调试 / 后续 pass 读
    GPUDescriptorHandle srv = bindless.Allocate();
    m_srvIndex = srv.index;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.NumElements = totalRays;
    srvDesc.Buffer.StructureByteStride = 64;
    device->CreateShaderResourceView(m_buffer.Get(), &srvDesc, srv.cpuHandle);
}