#include "UploadHeap.h"
#include "RenderContext.h"

UploadHeap::~UploadHeap() {
    if (m_uploadBuffer && m_cpuBase) {
        m_uploadBuffer->Unmap(0, nullptr);
        m_cpuBase = nullptr;
    }
}

void  UploadHeap::Init(uint64_t capacity) {
    ID3D12Device* device = RenderContext::Get().GetDevice();
    m_capacity = capacity;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

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