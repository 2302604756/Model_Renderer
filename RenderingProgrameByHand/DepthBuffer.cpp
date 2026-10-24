
#include "DepthBuffer.h"
#include "RenderContext.h"   // THROW_IF_FAILED

void DepthBuffer::Init(ID3D12Device* device, CPUDescriptorAllocator& dsvAllocator,
    uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kFormat;
    desc.SampleDesc = { 1, 0 };
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = kFormat;
    clearValue.DepthStencil.Depth = 1.0f;

    THROW_IF_FAILED(device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
        IID_PPV_ARGS(&m_resource)));

    CPUDescriptorHandle dsvSlot = dsvAllocator.DSVAllocate();
    m_dsv = dsvSlot.cpuHandle;

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = kFormat;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    device->CreateDepthStencilView(m_resource.Get(), &dsvDesc, m_dsv);
}

void DepthBuffer::Clear(ID3D12GraphicsCommandList* cmdList) const noexcept {
    cmdList->ClearDepthStencilView(m_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
}