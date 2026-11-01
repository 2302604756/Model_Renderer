#pragma once
#include <DirectXMath.h>
#include <cstdint>

// 每物体常量：32-bit root constants b0（40 DWORD = 160 B）
// HLSL 对应：Shaders.hlsl 的 struct PerObjectData
// 
//在循环 Pass1 Draw时从m_meshLibrary和m_objects中获取到具体数据，然后设置进入Constant Buffer Register
//包含mesh index和instance 信息
struct PerObjectConstants {
	DirectX::XMFLOAT4X4 world;        // 0   64（存转置矩阵）
	DirectX::XMFLOAT4X4 normalMatrix; // 64  64（world 的逆）
	DirectX::XMFLOAT4   albedo;       // 128 16
	uint32_t            vbIndex;      // 144 bindless VB 槽
	uint32_t            textureIndex; // 148（deferred 暂未使用）
	uint32_t            padding[2];   // 152 8
};
static_assert(sizeof(PerObjectConstants) == 160,
	"PerObjectConstants 必须是 40 个 32-bit 常量");