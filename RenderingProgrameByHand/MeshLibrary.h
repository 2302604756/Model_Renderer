#pragma once
#include <cstdint>
#include <vector>
#include "Mesh.h"
#include "TransientHeap.h"
#include "UploadHeap.h"
#include "BindlessDescriptorHeap.h"

// 几何库：持有所有 mesh，每个 mesh 的 VB/IB 都在 TransientHeap 里切块，
// 并在全局 Bindless 堆登记 SRV。渲染时只按 id 取 mesh 信息。
//可以通过Get来获取到对应索引的Mesh
class MeshLibrary {
public:
    uint32_t Add(const std::vector<Vertex>& vertices,
        const std::vector<uint32_t>& indices,
        TransientHeap& heap,
        UploadHeap& upload,
        GPUDescriptorAllocator& bindless);

    const Mesh& Get(uint32_t id) const { return m_meshes[id]; }
    uint32_t Count() const noexcept { return static_cast<uint32_t>(m_meshes.size()); }

private:
    std::vector<Mesh> m_meshes;
};