#include "Scene.h"

void Scene::Build(uint32_t cubeMeshId, uint32_t sphereMeshId, uint32_t textureIndex)
{
	m_objects.clear();

	auto addObject = [&](const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& scale,
		uint32_t meshId, const DirectX::XMFLOAT4& albedo) {
			RenderObject obj{};
			obj.position = pos;
			obj.scale = scale;
			obj.rotation = { 0.0f, 0.0f, 0.0f };
			obj.meshId = meshId;
			obj.textureIndex = textureIndex;
			obj.albedo = albedo;
			m_objects.push_back(obj);
		};

	const DirectX::XMFLOAT4 white{ 0.80f, 0.80f, 0.80f, 1.0f };
	const DirectX::XMFLOAT4 red{ 0.85f, 0.10f, 0.10f, 1.0f };
	const DirectX::XMFLOAT4 green{ 0.10f, 0.75f, 0.10f, 1.0f };
	const DirectX::XMFLOAT4 orange{ 0.95f, 0.55f, 0.15f, 1.0f };

	// Cornell Box（5 面墙 + 2 个球）
	addObject({ 0.0f, -0.05f, 0.0f }, { 5.0f, 0.1f, 5.0f }, cubeMeshId, white);
	addObject({ 0.0f,  2.55f, 0.0f }, { 5.0f, 0.1f, 5.0f }, cubeMeshId, white);
	addObject({ 0.0f,  1.25f, 2.55f }, { 5.0f, 2.6f, 0.1f }, cubeMeshId, white);
	addObject({ -2.55f, 1.25f, 0.0f }, { 0.1f, 2.6f, 5.0f }, cubeMeshId, red);
	addObject({ 2.55f, 1.25f, 0.0f }, { 0.1f, 2.6f, 5.0f }, cubeMeshId, green);
	addObject({ -0.8f, 0.70f, -0.6f }, { 1.0f, 1.0f, 1.0f }, sphereMeshId, white);
	addObject({ 0.9f, 0.46f,  0.3f }, { 0.65f, 0.65f, 0.65f }, sphereMeshId, orange);


	//==========================光源初始化=====================================
	m_lights.clear();

	Light sun;
	sun.type = LightType::Directional;
	sun.direction = { -0.5f, -0.45f, 0.9f };
	sun.color = { 1.0f, 1.0f, 1.0f };
	sun.intensity = 1.0f;
	m_lights.push_back(sun);

}