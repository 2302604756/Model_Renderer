#include "MeshLibrary.h"

#include <utility>

uint32_t MeshLibrary::Add(const std::vector<Vertex>& vertices,
    const std::vector<uint32_t>& indices,
    TransientHeap& heap,
    UploadHeap& upload,
    GPUDescriptorAllocator& bindless)
{
    Mesh mesh = Mesh::CreateFromDataPlaced(vertices, indices, heap, upload, bindless);
    m_meshes.push_back(std::move(mesh));
    return static_cast<uint32_t>(m_meshes.size() - 1);
}