#include "RenderTexture.h"
#include "RenderContext.h"   // THROW_IF_FAILED

void RenderTexture::Init(ID3D12Device* device,
    CPUDescriptorAllocator& rtvAllocator,
    GPUDescriptorAllocator& bindlessAllocator,
    uint32_t width, uint32_t height,
    DXGI_FORMAT format,
    const float clearColor[4])
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc = { 1,0 };
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = format;
    clear.Color[0] = clearColor[0];
    clear.Color[1] = clearColor[1];
    clear.Color[2] = clearColor[2];
    clear.Color[3] = clearColor[3];

    THROW_IF_FAILED(device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
        IID_PPV_ARGS(&m_resource)));
    m_state = D3D12_RESOURCE_STATE_RENDER_TARGET;

    // RTV（CPU 堆）
    CPUDescriptorHandle rtv = rtvAllocator.RTVAllocate();
    m_rtv = rtv.cpuHandle;
    device->CreateRenderTargetView(m_resource.Get(), nullptr, m_rtv);

    // SRV（bindless 堆），lighting pass 要用
    GPUDescriptorHandle srv = bindlessAllocator.Allocate();
    m_bindlessIndex = srv.index;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_resource.Get(), &srvDesc, srv.cpuHandle);

}

void RenderTexture::TransitionTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES target)
{
    if (m_state == target) return;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.pResource = m_resource.Get();
    barrier.Transition.StateBefore = m_state;
    barrier.Transition.StateAfter = target;
    cmdList->ResourceBarrier(1, &barrier);
    m_state = target;
}