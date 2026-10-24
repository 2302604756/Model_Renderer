#pragma once
#include <DirectXMath.h>
// 每帧常量：Root CBV b1（和 HLSL PerFrameData 一一对应）
struct PerFrameConstants {
	DirectX::XMFLOAT4X4 viewProj;    // 64
	DirectX::XMFLOAT4X4 invViewProj; // 64：延迟光照里由 depth 重建世界坐标
	DirectX::XMFLOAT4   cameraPos;   // 16
	float               time;        // 4
	float               padding[3];  // 12
};
static_assert(sizeof(PerFrameConstants) == 160, "PerFrameConstants 必须和 HLSL PerFrameData 布局一致");

// 每物体常量：32-bit root constants b0（36 DWORD = 144 字节，和 HLSL PerObjectData 对应）
struct PerObjectConstants {
	DirectX::XMFLOAT4X4 world;        // 64 世界矩阵（C++ 侧存转置，适配 HLSL mul(matrix, vector)）
	DirectX::XMFLOAT4X4 normalMatrix; // 64 法线矩阵（HLSL 列向量约定下是 world 的逆）
	DirectX::XMFLOAT4   albedo;       // 16
	uint32_t            vbIndex;      // 4 bindless VB SRV 槽位
	uint32_t            textureIndex; // 4 bindless 贴图 SRV 槽位
	uint32_t            padding[2];   // 8 
};
static_assert(sizeof(PerObjectConstants) == 160, "PerObjectConstants 必须是 40 个 32-bit 常量");

//LightingParams就是用来记录每个view在bindless槽位id的
//后续在shader中ResourceDescriptorHeap[id]就可以找到对应的数据
//eg：StructuredBuffer<GPUObjectData> objects = ResourceDescriptorHeap[g_Lighting.objectIndex];
struct LightingParams {
	uint32_t albedoIndex;
	uint32_t normalIndex;
	uint32_t depthIndex;
	uint32_t debugMode;
	uint32_t objectIndex;   // GPUObjectData buffer 的 bindless 槽
	uint32_t padding[3];
};
static_assert(sizeof(LightingParams) == 32);

// 每个实例在 GPU 侧的数据（instanceID == m_objects 下标）
struct GPUObjectData {
	DirectX::XMFLOAT4X4 world;        // 转置后的世界矩阵（HLSL mul(matrix, p)）
	DirectX::XMFLOAT4X4 normalMatrix; // world 的逆（法线用）
	DirectX::XMFLOAT4   albedo;
	uint32_t            vbIndex;      // bindless VB 槽
	uint32_t            ibIndex;      // bindless IB 槽
	uint32_t            indexCount;
	uint32_t            padding;
};
static_assert(sizeof(GPUObjectData) == 160);


// 场景里的一个物体（CPU 侧数据）
struct RenderObject {
	DirectX::XMFLOAT3 position{ 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 rotation{ 0.0f, 0.0f, 0.0f }; // RollPitchYaw，弧度
	DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };
	DirectX::XMFLOAT4 albedo{ 1.0f, 1.0f, 1.0f, 1.0f };
	uint32_t          meshId = 0;
	uint32_t          textureIndex = 0;
};
