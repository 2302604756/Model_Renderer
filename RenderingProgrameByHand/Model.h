#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>  // 必须包含 vector
#include <cmath>   // std::sin / std::cos
#include <utility> // std::swap
#include "Mesh.h"

static_assert(sizeof(Vertex) == 48, "MeshVertex 必须是 48 字节 stride");
static_assert(offsetof(Vertex, position) == 0);
static_assert(offsetof(Vertex, normal) == 12);
static_assert(offsetof(Vertex, uv) == 24);
static_assert(offsetof(Vertex, tangent) == 32);

struct MeshBuffer
{
    const Vertex* vertices;
    uint32_t vertexCount;
    uint32_t vertexStride;
    const uint32_t* indices;
    uint32_t indexCount;
};

inline const uint32_t kCubeVertexCount = 24;
inline const uint32_t kCubeIndexCount = 36;

// 如果你用的是 C++20，可以把 inline const 改成 inline constexpr
inline const std::vector<Vertex> kCubeVertices =
{
    // Front  N=( 0, 0,-1)
    { { -0.5f,  0.5f, -0.5f }, {  0.0f,  0.0f, -1.0f }, { 0.0f, 0.0f }, {  1.0f,  0.0f,  0.0f,  1.0f } },
    { {  0.5f,  0.5f, -0.5f }, {  0.0f,  0.0f, -1.0f }, { 1.0f, 0.0f }, {  1.0f,  0.0f,  0.0f,  1.0f } },
    { {  0.5f, -0.5f, -0.5f }, {  0.0f,  0.0f, -1.0f }, { 1.0f, 1.0f }, {  1.0f,  0.0f,  0.0f,  1.0f } },
    { { -0.5f, -0.5f, -0.5f }, {  0.0f,  0.0f, -1.0f }, { 0.0f, 1.0f }, {  1.0f,  0.0f,  0.0f,  1.0f } },
    // Back   N=( 0, 0, 1)
    { {  0.5f,  0.5f,  0.5f }, {  0.0f,  0.0f,  1.0f }, { 0.0f, 0.0f }, { -1.0f,  0.0f,  0.0f,  1.0f } },
    { { -0.5f,  0.5f,  0.5f }, {  0.0f,  0.0f,  1.0f }, { 1.0f, 0.0f }, { -1.0f,  0.0f,  0.0f,  1.0f } },
    { { -0.5f, -0.5f,  0.5f }, {  0.0f,  0.0f,  1.0f }, { 1.0f, 1.0f }, { -1.0f,  0.0f,  0.0f,  1.0f } },
    { {  0.5f, -0.5f,  0.5f }, {  0.0f,  0.0f,  1.0f }, { 0.0f, 1.0f }, { -1.0f,  0.0f,  0.0f,  1.0f } },
    // Left   N=(-1, 0, 0)
    { { -0.5f,  0.5f,  0.5f }, { -1.0f,  0.0f,  0.0f }, { 0.0f, 0.0f }, {  0.0f,  0.0f, -1.0f,  1.0f } },
    { { -0.5f,  0.5f, -0.5f }, { -1.0f,  0.0f,  0.0f }, { 1.0f, 0.0f }, {  0.0f,  0.0f, -1.0f,  1.0f } },
    { { -0.5f, -0.5f, -0.5f }, { -1.0f,  0.0f,  0.0f }, { 1.0f, 1.0f }, {  0.0f,  0.0f, -1.0f,  1.0f } },
    { { -0.5f, -0.5f,  0.5f }, { -1.0f,  0.0f,  0.0f }, { 0.0f, 1.0f }, {  0.0f,  0.0f, -1.0f,  1.0f } },
    // Right  N=( 1, 0, 0)
    { {  0.5f,  0.5f, -0.5f }, {  1.0f,  0.0f,  0.0f }, { 0.0f, 0.0f }, {  0.0f,  0.0f,  1.0f,  1.0f } },
    { {  0.5f,  0.5f,  0.5f }, {  1.0f,  0.0f,  0.0f }, { 1.0f, 0.0f }, {  0.0f,  0.0f,  1.0f,  1.0f } },
    { {  0.5f, -0.5f,  0.5f }, {  1.0f,  0.0f,  0.0f }, { 1.0f, 1.0f }, {  0.0f,  0.0f,  1.0f,  1.0f } },
    { {  0.5f, -0.5f, -0.5f }, {  1.0f,  0.0f,  0.0f }, { 0.0f, 1.0f }, {  0.0f,  0.0f,  1.0f,  1.0f } },
    // Top    N=( 0, 1, 0)
    { {  0.5f,  0.5f, -0.5f }, {  0.0f,  1.0f,  0.0f }, { 0.0f, 0.0f }, { -1.0f,  0.0f,  0.0f,  1.0f } },
    { { -0.5f,  0.5f, -0.5f }, {  0.0f,  1.0f,  0.0f }, { 1.0f, 0.0f }, { -1.0f,  0.0f,  0.0f,  1.0f } },
    { { -0.5f,  0.5f,  0.5f }, {  0.0f,  1.0f,  0.0f }, { 1.0f, 1.0f }, { -1.0f,  0.0f,  0.0f,  1.0f } },
    { {  0.5f,  0.5f,  0.5f }, {  0.0f,  1.0f,  0.0f }, { 0.0f, 1.0f }, { -1.0f,  0.0f,  0.0f,  1.0f } },
    // Bottom N=( 0,-1, 0)
    { {  0.5f, -0.5f,  0.5f }, {  0.0f, -1.0f,  0.0f }, { 0.0f, 0.0f }, { -1.0f,  0.0f,  0.0f,  1.0f } },
    { { -0.5f, -0.5f,  0.5f }, {  0.0f, -1.0f,  0.0f }, { 1.0f, 0.0f }, { -1.0f,  0.0f,  0.0f,  1.0f } },
    { { -0.5f, -0.5f, -0.5f }, {  0.0f, -1.0f,  0.0f }, { 1.0f, 1.0f }, { -1.0f,  0.0f,  0.0f,  1.0f } },
    { {  0.5f, -0.5f, -0.5f }, {  0.0f, -1.0f,  0.0f }, { 0.0f, 1.0f }, { -1.0f,  0.0f,  0.0f,  1.0f } },
};

inline const std::vector<uint32_t> kCubeIndices =
{
     0,  3,  2,   0,  2,  1,   // Front
     4,  7,  6,   4,  6,  5,   // Back
     8, 11, 10,   8, 10,  9,   // Left
     12, 15, 14,  12, 14, 13,  // Right
     16, 19, 18,  16, 18, 17,  // Top
     20, 23, 22,  20, 22, 21,  // Bottom
};

// ⚠️ 注意：这里必须使用 .data() 来获取 vector 的底层指针
inline const MeshBuffer kCubeMesh =
{
    kCubeVertices.data(),  // 获取底层数组指针
    kCubeVertexCount,
    static_cast<uint32_t>(sizeof(Vertex)),
    kCubeIndices.data(),   // 获取底层数组指针
    kCubeIndexCount
}; 


// =============================================================================
// 程序化几何：球 / 平面
// =============================================================================

// 生成器统一按"右手叉积法线朝外"排三角形；D3D 左手系 + FrontCounterClockwise=TRUE
// 的约定正好相反（正面三角形的右手叉积指向物体内部），所以这里统一翻转一次，
// 保证和 kCubeIndices 的绕序约定一致。
inline void EnsureLeftHandedWinding(const std::vector<Vertex>& vertices, std::vector<uint32_t>& indices)
{
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const Vertex& a = vertices[indices[i]];
        const Vertex& b = vertices[indices[i + 1]];
        const Vertex& c = vertices[indices[i + 2]];

        const float e1[3] = { b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2] };
        const float e2[3] = { c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2] };
        const float n[3] = {
            e1[1] * e2[2] - e1[2] * e2[1],
            e1[2] * e2[0] - e1[0] * e2[2],
            e1[0] * e2[1] - e1[1] * e2[0] };

        const float outward[3] = {
            a.normal[0] + b.normal[0] + c.normal[0],
            a.normal[1] + b.normal[1] + c.normal[1],
            a.normal[2] + b.normal[2] + c.normal[2] };

        if (n[0] * outward[0] + n[1] * outward[1] + n[2] * outward[2] > 0.0f) {
            std::swap(indices[i + 1], indices[i + 2]);
        }
    }
}

inline void CreateSphereMesh(float radius, uint32_t slices, uint32_t stacks,
    std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices)
{
    outVertices.clear();
    outIndices.clear();

    const float pi = 3.14159265358979323846f;
    const float twoPi = 2.0f * pi;

    for (uint32_t y = 0; y <= stacks; ++y) {
        const float v = static_cast<float>(y) / static_cast<float>(stacks);
        const float phi = v * pi;
        for (uint32_t x = 0; x <= slices; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(slices);
            const float theta = u * twoPi;
            const float sinPhi = std::sin(phi), cosPhi = std::cos(phi);
            const float sinTheta = std::sin(theta), cosTheta = std::cos(theta);

            Vertex vert{};
            vert.position[0] = radius * sinPhi * sinTheta;
            vert.position[1] = radius * cosPhi;
            vert.position[2] = radius * sinPhi * cosTheta;
            vert.normal[0] = sinPhi * sinTheta;
            vert.normal[1] = cosPhi;
            vert.normal[2] = sinPhi * cosTheta;
            vert.uv[0] = u;
            vert.uv[1] = 1.0f - v;
            vert.tangent[0] = 1.0f;
            vert.tangent[3] = 1.0f;
            outVertices.push_back(vert);
        }
    }

    for (uint32_t y = 0; y < stacks; ++y) {
        for (uint32_t x = 0; x < slices; ++x) {
            const uint32_t i0 = y * (slices + 1) + x;
            const uint32_t i1 = i0 + 1;
            const uint32_t i2 = i0 + (slices + 1);
            const uint32_t i3 = i2 + 1;

            outIndices.push_back(i0); outIndices.push_back(i2); outIndices.push_back(i1);
            outIndices.push_back(i1); outIndices.push_back(i2); outIndices.push_back(i3);
        }
    }

    EnsureLeftHandedWinding(outVertices, outIndices);
}

inline void CreatePlaneMesh(float size, uint32_t segments,
    std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices)
{
    outVertices.clear();
    outIndices.clear();

    for (uint32_t y = 0; y <= segments; ++y) {
        const float fy = static_cast<float>(y) / static_cast<float>(segments);
        for (uint32_t x = 0; x <= segments; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(segments);

            Vertex vert{};
            vert.position[0] = (fx - 0.5f) * size;
            vert.position[1] = 0.0f;
            vert.position[2] = (fy - 0.5f) * size;
            vert.normal[1] = 1.0f;
            vert.uv[0] = fx;
            vert.uv[1] = fy;
            vert.tangent[0] = 1.0f;
            vert.tangent[3] = 1.0f;
            outVertices.push_back(vert);
        }
    }

    const uint32_t rowSize = segments + 1;
    for (uint32_t y = 0; y < segments; ++y) {
        for (uint32_t x = 0; x < segments; ++x) {
            const uint32_t i0 = y * rowSize + x;
            const uint32_t i1 = i0 + 1;
            const uint32_t i2 = i0 + rowSize;
            const uint32_t i3 = i2 + 1;

            outIndices.push_back(i0); outIndices.push_back(i2); outIndices.push_back(i1);
            outIndices.push_back(i1); outIndices.push_back(i2); outIndices.push_back(i3);
        }
    }

    EnsureLeftHandedWinding(outVertices, outIndices);
}