#include "SceneGpuData.h"
#include "Scene.h"
#include "MeshLibrary.h"
#include "InstanceData.h"
#include "RenderContext.h"   // THROW_IF_FAILED
#include <cstring>
#include <vector>

void SceneGpuData::Build(ID3D12Device* device,
	GPUDescriptorAllocator& bindless,
	const MeshLibrary& meshLibrary,
	const Scene& scene)
{
	const std::vector<RenderObject>& objects = scene.GetObjects();

	std::vector<GPUObjectData> gpuObjects(objects.size());
	for (size_t i = 0; i < objects.size(); ++i) {
		const RenderObject& obj = objects[i];
		const DirectX::XMMATRIX world =
			DirectX::XMMatrixScaling(obj.scale.x, obj.scale.y, obj.scale.z) *
			DirectX::XMMatrixRotationRollPitchYaw(obj.rotation.x, obj.rotation.y, obj.rotation.z) *
			DirectX::XMMatrixTranslation(obj.position.x, obj.position.y, obj.position.z);

		GPUObjectData& o = gpuObjects[i];
		DirectX::XMStoreFloat4x4(&o.world, DirectX::XMMatrixTranspose(world));
		DirectX::XMStoreFloat4x4(&o.normalMatrix, DirectX::XMMatrixInverse(nullptr, world));
		o.albedo = obj.albedo;

		const Mesh& mesh = meshLibrary.Get(obj.meshId);
		o.vbIndex = mesh.GetVBBindlessIndex();
		o.ibIndex = mesh.GetIBBindlessIndex();
		o.indexCount = mesh.GetIndexCount();
		o.padding = 0;
	}

	const uint64_t bytes = gpuObjects.size() * sizeof(GPUObjectData);
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = bytes;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc = { 1, 0 };
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	D3D12_HEAP_PROPERTIES props{};
	props.Type = D3D12_HEAP_TYPE_GPU_UPLOAD;
	THROW_IF_FAILED(device->CreateCommittedResource(
		&props, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		IID_PPV_ARGS(&m_buffer)));

	void* mapped = nullptr;
	THROW_IF_FAILED(m_buffer->Map(0, nullptr, &mapped));
	std::memcpy(mapped, gpuObjects.data(), bytes);
	m_buffer->Unmap(0, nullptr);

	GPUDescriptorHandle handle = bindless.Allocate();
	m_bindlessIndex = handle.index;

	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_UNKNOWN;
	srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Buffer.FirstElement = 0;
	srv.Buffer.NumElements = static_cast<UINT>(gpuObjects.size());
	srv.Buffer.StructureByteStride = sizeof(GPUObjectData);
	device->CreateShaderResourceView(m_buffer.Get(), &srv, handle.cpuHandle);
}