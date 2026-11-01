#pragma once
#include <DirectXMath.h>
#include <cstdint>

// 每个实例的 GPU 数据：bindless StructuredBuffer<GPUObjectData>
// HLSL 对应：Shaders.hlsl 的 struct GPUObjectData
// instanceID == RenderObject 在 m_objects 中的下标

//在BuildInstance中使用 从m_objects和m_meshlibrary 中获得具体每instance的数据 并且memcpy到具体的gpu heap
//并写入Descriptor Heap中
//无法直接使用 而是使用bindless索引
struct GPUObjectData {
	DirectX::XMFLOAT4X4 world;        // 0   64（转置）
	DirectX::XMFLOAT4X4 normalMatrix; // 64  64（world 的逆）
	DirectX::XMFLOAT4   albedo;       // 128 16
	uint32_t            vbIndex;      // 144 bindless VB 槽
	uint32_t            ibIndex;      // 148 bindless IB 槽
	uint32_t            indexCount;   // 152
	uint32_t            padding;      // 156
};
static_assert(sizeof(GPUObjectData) == 160,
	"GPUObjectData 必须和 HLSL 布局一致");