// Mesh.h
#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <cstdint>
#include "GpuMemoryBlock.h"
#include "TransientHeap.h"
#include "UploadHeap.h"
#include "BindlessDescriptorHeap.h"

using Microsoft::WRL::ComPtr;

//我们直接将传统绑定和Bindless绑定都做进去

// 标准 PBR 顶点结构 (48 Bytes，完美对齐缓存行)
struct Vertex {
    float position[3]; // 12 Bytes
    float normal[3];   // 12 Bytes
    float uv[2];       // 8 Bytes
    float tangent[4];  // 16 Bytes (xyz: 切线方向, w: 副切线手性符号)
};

class Mesh {
public:
    static Mesh CreateFromDataPlaced(
        const std::vector<Vertex>& vertices,
        const std::vector<uint32_t>& indices,
        TransientHeap& defaultHeap,
        UploadHeap& uploadHeap,
        GPUDescriptorAllocator& bindlessAlloctor);

    //供传统光栅化IA阶段绑定
    const D3D12_VERTEX_BUFFER_VIEW& GetVBV() const noexcept { return m_vbv; }
    const D3D12_INDEX_BUFFER_VIEW& GetIBV() const noexcept { return m_ibv; }
    uint32_t GetIndexCount() const noexcept { return m_indexCount; }

    // 供 Bindless Vertex Pulling 及后续 DDGI RayQuery 直接寻址顶点/索引
    uint32_t GetVBBindlessIndex() const noexcept { return m_vbBindlessIndex; }
    uint32_t GetIBBindlessIndex() const noexcept { return m_ibBindlessIndex; }

    ID3D12Resource* GetVertexBuffer() const noexcept { return m_vertexBuffer.Get(); }
    ID3D12Resource* GetIndexBuffer()  const noexcept { return m_indexBuffer.Get(); }

    D3D12_GPU_VIRTUAL_ADDRESS GetVertexBufferGPUAddress() const noexcept { return m_vertexBuffer->GetGPUVirtualAddress(); }

    uint32_t GetVertexCount() const noexcept { return m_vertexCount; }
    D3D12_GPU_VIRTUAL_ADDRESS GetIndexBufferGPUAddress() const noexcept { return m_indexBuffer->GetGPUVirtualAddress(); }

private:
    ComPtr<ID3D12Resource>   m_vertexBuffer;
    ComPtr<ID3D12Resource>   m_indexBuffer;
    GpuMemoryBlock           m_vbMemBlock;
    GpuMemoryBlock           m_ibMemBlock;

    D3D12_VERTEX_BUFFER_VIEW m_vbv{};
    D3D12_INDEX_BUFFER_VIEW  m_ibv{};
    uint32_t                 m_vertexCount = 0;
    uint32_t                 m_indexCount = 0;

    // 全局 Bindless 堆中的 StructuredBuffer SRV 索引
    uint32_t                 m_vbBindlessIndex = UINT32_MAX;
    uint32_t                 m_ibBindlessIndex = UINT32_MAX;
    
};