#pragma once
#include <DirectXMath.h>

// 每帧常量：Root CBV b1
// HLSL 对应：Shaders.hlsl 的 struct PerFrameData（布局必须一致）
// 在App.cpp中每帧常量更新中写入，设置进入Constant Buffer Register
struct PerFrameConstants {
	DirectX::XMFLOAT4X4 viewProj;    // 0   64
	DirectX::XMFLOAT4X4 invViewProj; // 64  64（由 depth 重建世界坐标）
	DirectX::XMFLOAT4   cameraPos;   // 128 16
	float               time;        // 144 4
	uint32_t            frameIndex;  // 递增帧号
	float               padding[2];  // 148 12
};
static_assert(sizeof(PerFrameConstants) == 160,
	"PerFrameConstants 必须和 HLSL PerFrameData 布局一致");