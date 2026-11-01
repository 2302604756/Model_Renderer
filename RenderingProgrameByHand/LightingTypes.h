#pragma once
#include <cstdint>
#include <DirectXMath.h>

// Lighting pass 参数：root constants b2（8 DWORD = 32 B）
// HLSL 对应：Shaders.hlsl 的 struct LightingParams

//定义Pass 1 GBuffer pass 中的OutPut槽位供Pass 2使用 设置进Constant Buffer Register
struct LightingParams {
	uint32_t albedoIndex;   // G-buffer albedo 的 bindless SRV 槽
	uint32_t normalIndex;   // G-buffer normal 的 bindless SRV 槽
	uint32_t depthIndex;    // depth SRV 的 bindless 槽
	uint32_t debugMode;     // 0 final, 1 albedo, 2 normal, 3 depth, 4 worldpos, 5 shadow, 6 GI
	uint32_t objectIndex;   // GPUObjectData buffer 的 bindless 槽
	uint32_t lightCount;     // 新增：光源数量
	uint32_t computeTextureIndex;   // Compute Shaders
	uint32_t distanceAtlasIndex;    // 距离矩 atlas 的 bindless SRV 槽
	DirectX::XMFLOAT4 gridOriginSpacing;       // xyz origin, w spacing
	uint32_t gridCountX;
	uint32_t gridCountY;
	uint32_t gridCountZ;
	uint32_t probeSrvIndex;
};
static_assert(sizeof(LightingParams) == 64);

// 太阳 / 天空 / 环境光：计划 b3 CBV（阶段 3 接线，当前未使用）
// HLSL 对应：Shaders.hlsl 的 cbuffer LightingCB : register(b3)

// 在BuildLight中从
struct LightingConstants {
	DirectX::XMFLOAT4 sunDirection; // xyz: 光传播方向, w: 强度
	DirectX::XMFLOAT4 sunColor;     // rgb, w = 1
	DirectX::XMFLOAT4 skyColor;     // 天空 / 环境色
	DirectX::XMFLOAT4 ambient;      // 环境光
};
static_assert(sizeof(LightingConstants) == 64);

// 光源数组的 GPU 布局：计划 t5 root SRV（阶段 3 接线，当前未使用）
// HLSL 对应结构尚未定义
struct GPULight {
	DirectX::XMFLOAT4 positionType;    // xyz = 位置, w = LightType
	DirectX::XMFLOAT4 directionRange;  // xyz = 传播方向, w = range
	DirectX::XMFLOAT4 colorIntensity;  // rgb = 颜色, w = 强度
	DirectX::XMFLOAT4 coneParams;      // x = cos(outer), y = cos(inner)
};
static_assert(sizeof(GPULight) == 64);