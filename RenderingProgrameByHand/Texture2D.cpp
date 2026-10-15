#include "Texture2D.h"
#include "RenderContext.h"
#include "dds_loader.h"

Texture2D Texture2D::CreatePlaced(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& desc,
    const GpuMemoryBlock& memoryBlock,
    D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue)
{
    Texture2D tex;
    tex.m_memoryBlock = memoryBlock;
    tex.m_currentState = initialState;

    // 直接调用底层 API 将资源挂载在指定的物理偏移上
    THROW_IF_FAILED(device->CreatePlacedResource(
        memoryBlock.heap,
        memoryBlock.offset,
        &desc,
        initialState,
        clearValue,
        IID_PPV_ARGS(&tex.m_resource)
    ));

    return tex;
}


Texture2D Texture2D::LoadFromFilePlaced(
    const std::string& path,
    TransientHeap& defaultHeap,
    UploadHeap& uploadHeap,
    GPUDescriptorAllocator& bindlessAllocator
    ) {

    ID3D12Device* device = RenderContext::Get().GetDevice();
    ComPtr<ID3D12GraphicsCommandList> cmdList = RenderContext::Get().GetFrame().cmdList;

    //1.解析 DDS 文件
    dds::ImageData image = dds::LoadFromFile(path);
    
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = (image.depth > 1) ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
        : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = image.width;
    texDesc.Height = image.height;
    texDesc.DepthOrArraySize = (image.depth > 1) ? static_cast<UINT16>(image.depth) 
        : static_cast<UINT16>(image.arraySize);
    texDesc.MipLevels = static_cast<UINT16> (image.mipCount);
    texDesc.Format = image.format;
    texDesc.SampleDesc = { 1, 0 };
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    //2.从默认物理堆申请空间 并创建PlacedResource
    GpuMemoryBlock memBlock = defaultHeap.Allocate(texDesc);
    Texture2D tex = CreatePlaced(device, texDesc, memBlock, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);

    //3.从texDesc获取到footprint布局
    const UINT numSubresources = static_cast<UINT>(image.subresources.size());
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(numSubresources);
    uint64_t uploadSize = 0;
    std::vector<UINT>   numRows(numSubresources);
    std::vector<UINT64> rowSizesInBytes(numSubresources);
    
    device->GetCopyableFootprints(&texDesc, 0, numSubresources, 0, layouts.data(), numRows.data(), rowSizesInBytes.data(), &uploadSize);

    //申请上传堆空间
    UploadBlock uploadBlock = uploadHeap.Allocate(uploadSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

    // 4. 将像素 memcpy 到 UploadBlock 的常驻映射指针中
    for (UINT i = 0; i < numSubresources; ++i) {
        const auto& src = image.subresources[i];
        auto& dstLayout = layouts[i];

        uint8_t* dstSlice = uploadBlock.cpuAddress + dstLayout.Offset;

        for (uint32_t y = 0; y < numRows[i]; ++y) {
            std::memcpy(dstSlice + y * dstLayout.Footprint.RowPitch, src.data + y * src.rowPitch, src.rowPitch);
        }
   
        // 关键：将 Footprint 的相对偏移加上 UploadBlock 在大 Buffer 中的基址偏移！
        dstLayout.Offset += uploadBlock.offset;

        // 5. 录制拷贝指令
        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = tex.m_resource.Get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = i;

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = uploadBlock.buffer;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = dstLayout;

        cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    }

    // 6. 状态屏障：COPY_DEST -> PIXEL_SHADER_RESOURCE
    D3D12_RESOURCE_BARRIER barrier = d3d12lite::CreateTransitionBarrier(
        tex.m_resource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
    cmdList->ResourceBarrier(1, &barrier);
    tex.m_currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // 7. 直写全局 Bindless 堆
    GPUDescriptorHandle bindlessHandle = bindlessAllocator.Allocate();
    tex.m_bindlessIndex = bindlessHandle.index;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = texDesc.MipLevels;
    device->CreateShaderResourceView(tex.m_resource.Get(), &srvDesc, bindlessHandle.cpuHandle);

    return tex;
}