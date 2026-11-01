#pragma once
#include <cstdint>
#include <DirectXMath.h>

// 场景里的一个物体（CPU 侧数据，不含 GPU 资源）
//存储场景中的单个物体非Mesh信息 不包含具体的Mesh信息 在Build Scene中初始化 绑定 
//不直接绑定进GPU端 中间数据结构体
struct RenderObject {
	DirectX::XMFLOAT3 position{ 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 rotation{ 0.0f, 0.0f, 0.0f }; // RollPitchYaw，弧度
	DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };
	DirectX::XMFLOAT4 albedo{ 1.0f, 1.0f, 1.0f, 1.0f };
	uint32_t          meshId = 0;
	uint32_t          textureIndex = 0;
};

// CPU 侧光源（阶段 3 由 BuildLights 转成 GPULight[]）
enum class LightType : uint32_t { Directional = 0, Point = 1, Spot = 2 };

struct Light {
	LightType type = LightType::Directional;
	DirectX::XMFLOAT3 position{ 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 direction{ 0.0f, -1.0f, 0.0f }; // 光传播方向
	DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
	float intensity = 1.0f;
	float range = 10.0f;        // point / spot 用
	float innerCone = 0.0f;     // 弧度，spot 用
	float outerCone = 0.0f;
};