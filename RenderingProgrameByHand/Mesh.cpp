// Mesh.cpp 核心创建与上传逻辑
#include "Mesh.h"
#include "RenderContext.h"
#include "d3d12lite.h"

Mesh Mesh::CreateFromDataPlaced(
    const std::vector<Vertex>& vertices,
    const std::vector<uint32_t>& indices,
    TransientHeap& defaultHeap,
    UploadHeap& uploadHeap,
    GPUDescriptorAllocator& bindlessAllocator)
{
    ID3D12Device* device = RenderContext::Get().GetDevice();
    ID3D12GraphicsCommandList* cmdList = RenderContext::Get().GetFrame().cmdList.Get();

    Mesh mesh{};
    mesh.m_vertexCount = static_cast<uint32_t>(vertices.size());
    mesh.m_indexCount = static_cast<uint32_t>(indices.size());

    const uint64_t vbByteSize = mesh.m_vertexCount * sizeof(Vertex);
    const uint64_t ibByteSize = mesh.m_indexCount * sizeof(uint32_t);

    // 1. 构造通用 Buffer 描述符辅助 lambda
    auto makeBufferDesc = [](uint64_t byteSize) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = byteSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc = { 1, 0 };
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return desc;
        };

    D3D12_RESOURCE_DESC vbDesc = makeBufferDesc(vbByteSize);
    D3D12_RESOURCE_DESC ibDesc = makeBufferDesc(ibByteSize);

    // 2. 在 Default 物理堆上分配 Placed 空间并创建资源 (初始状态为 COPY_DEST)
    mesh.m_vbMemBlock = defaultHeap.Allocate(vbDesc);
    mesh.m_ibMemBlock = defaultHeap.Allocate(ibDesc);

    THROW_IF_FAILED(device->CreatePlacedResource(
        mesh.m_vbMemBlock.heap, mesh.m_vbMemBlock.offset, &vbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mesh.m_vertexBuffer)
    ));
    THROW_IF_FAILED(device->CreatePlacedResource(
        mesh.m_ibMemBlock.heap, mesh.m_ibMemBlock.offset, &ibDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mesh.m_indexBuffer)
    ));

    // 3. 从 UploadHeap 切出中转内存并写入 CPU 顶点/索引数据
    UploadBlock vbUpload = uploadHeap.Allocate(vbByteSize, 256);
    UploadBlock ibUpload = uploadHeap.Allocate(ibByteSize, 256);

    std::memcpy(vbUpload.cpuAddress, vertices.data(), vbByteSize);//非常好用啊，只需要从uploadHeap申请空间然后去memcpy即可
    std::memcpy(ibUpload.cpuAddress, indices.data(), ibByteSize);

    // 4. 录制 DMA 线性拷贝指令 (Upload -> Default)
    cmdList->CopyBufferRegion(mesh.m_vertexBuffer.Get(), 0, vbUpload.buffer, vbUpload.offset, vbByteSize);
    cmdList->CopyBufferRegion(mesh.m_indexBuffer.Get(), 0, ibUpload.buffer, ibUpload.offset, ibByteSize);

    // 5. 状态屏障：COPY_DEST -> GENERIC_READ (同时支持 IA 顶点/索引读取与 Shader SRV 读取)
    D3D12_RESOURCE_BARRIER barriers[2] = {
        d3d12lite::CreateTransitionBarrier(mesh.m_vertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ),
        d3d12lite::CreateTransitionBarrier(mesh.m_indexBuffer.Get(),  D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ)
    };
    cmdList->ResourceBarrier(2, barriers);

    // 6. 填写传统 IA 视图 (VBV / IBV)
    mesh.m_vbv.BufferLocation = mesh.m_vertexBuffer->GetGPUVirtualAddress();
    mesh.m_vbv.SizeInBytes = static_cast<UINT>(vbByteSize);
    mesh.m_vbv.StrideInBytes = sizeof(Vertex);

    mesh.m_ibv.BufferLocation = mesh.m_indexBuffer->GetGPUVirtualAddress();
    mesh.m_ibv.SizeInBytes = static_cast<UINT>(ibByteSize);
    mesh.m_ibv.Format = DXGI_FORMAT_R32_UINT;

    // 7. 注册到全局 Bindless 堆 (StructuredBuffer SRV，供 Vertex Pulling 或 DDGI 光追读取)
    GPUDescriptorHandle vbSrvHandle = bindlessAllocator.Allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC vbSrvDesc{};
    vbSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    vbSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    vbSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    vbSrvDesc.Buffer.FirstElement = 0;
    vbSrvDesc.Buffer.NumElements = mesh.m_vertexCount;
    vbSrvDesc.Buffer.StructureByteStride = sizeof(Vertex);
    device->CreateShaderResourceView(mesh.m_vertexBuffer.Get(), &vbSrvDesc, vbSrvHandle.cpuHandle);
    mesh.m_vbBindlessIndex = vbSrvHandle.index;

    GPUDescriptorHandle ibSrvHandle = bindlessAllocator.Allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC ibSrvDesc = vbSrvDesc;
    ibSrvDesc.Buffer.NumElements = mesh.m_indexCount;
    ibSrvDesc.Buffer.StructureByteStride = sizeof(uint32_t);
    device->CreateShaderResourceView(mesh.m_indexBuffer.Get(), &ibSrvDesc, ibSrvHandle.cpuHandle);
    mesh.m_ibBindlessIndex = ibSrvHandle.index;

    return mesh;
}