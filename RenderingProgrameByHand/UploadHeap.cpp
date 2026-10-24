#include "UploadHeap.h"
#include "RenderContext.h"

UploadHeap::~UploadHeap() {
    if (m_uploadBuffer && m_cpuBase) {
        m_uploadBuffer->Unmap(0, nullptr);
        m_cpuBase = nullptr;
    }
}

void  UploadHeap::Init(uint64_t capacity,bool preferGpuUpload) {
    ID3D12Device* device = RenderContext::Get().GetDevice();
    m_capacity = capacity;
    
    // 新增： 检测硬件与 BIOS 是否支持 D3D12_HEAP_TYPE_GPU_UPLOAD (ReBAR) 同时可以手动设置
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16{};
    if (preferGpuUpload && SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options16, sizeof(options16)))) {
        m_isGpuUpload = (options16.GPUUploadHeapSupported == TRUE);
    }

    //判断是否使用GPU_UPLOAD
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = m_isGpuUpload ? D3D12_HEAP_TYPE_GPU_UPLOAD : D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width            = capacity;
    bufferDesc.Height           = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels        = 1;
    bufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc       = { 1,0 };
    bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    THROW_IF_FAILED(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_uploadBuffer)
    ));

    // 常驻映射：整个生命周期只 Map 一次
    D3D12_RANGE readRange{ 0, 0 }; // CPU 不读取该内存
    THROW_IF_FAILED(m_uploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_cpuBase)));
    m_gpuBase = m_uploadBuffer->GetGPUVirtualAddress();
    
    m_currentOffset = 0;
}

UploadBlock UploadHeap::Allocate(uint64_t sizeInBytes, uint64_t alignment) {
    uint64_t alignedOffset = (m_currentOffset + alignment) &~(alignment - 1);
    assert(alignedOffset + sizeInBytes <= m_capacity && "UploadHeap Out of Memory!");

    m_currentOffset = alignedOffset + sizeInBytes;

    UploadBlock block{};
    block.buffer = m_uploadBuffer.Get();
    block.cpuAddress = m_cpuBase + alignedOffset;
    block.gpuAddress = m_gpuBase + alignedOffset;
    block.offset = alignedOffset;
    block.size = sizeInBytes;
    return block;
}