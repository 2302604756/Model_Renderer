#pragma once
#include <cstdint>
#include <vector>
#include "SceneTypes.h"

// 场景内容（CPU 侧）：物体 + 光源。只负责"描述场景"，不持有 GPU 资源。
class Scene {
public:
	// mesh / 贴图槽由 App 创建后传入（以后可以换成资源句柄）
	void Build(uint32_t cubeMeshId, uint32_t sphereMeshId, uint32_t textureIndex);

	const std::vector<RenderObject>& GetObjects() const noexcept { return m_objects; }
	const std::vector<Light>& GetLights()  const noexcept { return m_lights; }

private:
	std::vector<RenderObject> m_objects;
	std::vector<Light>        m_lights;   // 阶段 3 光源系统使用
};