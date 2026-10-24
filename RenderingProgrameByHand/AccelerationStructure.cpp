#include "AccelerationStructure.h"
#include "RenderContext.h"   // THROW_IF_FAILED
#include <DirectXMath.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>



namespace rt {
	namespace {
        ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, uint64_t size,
            D3D12_HEAP_TYPE heapType,
            D3D12_RESOURCE_STATES initialState,
            D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = std::max<uint64_t>(size, 256);
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc = { 1, 0 };
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = flags;        

            D3D12_HEAP_PROPERTIES props{};
            props.Type = heapType;

            ComPtr<ID3D12Resource> resource;
            THROW_IF_FAILED(device->CreateCommittedResource(
                &props, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                IID_PPV_ARGS(&resource)));
            return resource;
        }

        void InsertUavBarrier(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* resource)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = resource;
            cmdList->ResourceBarrier(1, &barrier);
        }

        AccelerationStructure BuildBLASImpl(
            ID3D12Device5* device,
            ID3D12GraphicsCommandList4* cmdList,
            ID3D12Resource* vertexBuffer, uint32_t vertexCount, uint32_t vertexStride,
            ID3D12Resource* indexBuffer, uint32_t indexCount)
        {
            D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
            geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            geometry.Triangles.VertexBuffer.StartAddress = vertexBuffer->GetGPUVirtualAddress();
            geometry.Triangles.VertexBuffer.StrideInBytes = vertexStride;
            geometry.Triangles.VertexCount = vertexCount;
            geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geometry.Triangles.IndexBuffer = indexBuffer->GetGPUVirtualAddress();
            geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            geometry.Triangles.IndexCount = indexCount;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            inputs.NumDescs = 1;
            inputs.pGeometryDescs = &geometry;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
            device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
            if (prebuild.ResultDataMaxSizeInBytes == 0)
                throw std::runtime_error("BLAS prebuild info 无效");

            AccelerationStructure data;
            data.resource = CreateBuffer(device, prebuild.ResultDataMaxSizeInBytes,
                D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS); 
            data.scratch = CreateBuffer(device, prebuild.ScratchDataSizeInBytes,
                D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
            build.Inputs = inputs;
            build.ScratchAccelerationStructureData = data.scratch->GetGPUVirtualAddress();
            build.DestAccelerationStructureData = data.resource->GetGPUVirtualAddress();

            cmdList->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
            InsertUavBarrier(cmdList, data.resource.Get());

            data.gpuAddress = data.resource->GetGPUVirtualAddress();
            return data;
        }

        AccelerationStructure BuildTLASImpl(
            ID3D12Device5* device,
            ID3D12GraphicsCommandList4* cmdList,
            const std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& instances)
        {
            if (instances.empty())
                throw std::runtime_error("TLAS 实例为空");

            const UINT count = static_cast<UINT>(instances.size());
            const uint64_t instanceBytes = static_cast<uint64_t>(count) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);

            auto instanceBuffer = CreateBuffer(device, instanceBytes,
                D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

            void* mapped = nullptr;
            THROW_IF_FAILED(instanceBuffer->Map(0, nullptr, &mapped));
            std::memcpy(mapped, instances.data(), instanceBytes);
            instanceBuffer->Unmap(0, nullptr);

            //建树策略
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{}; 
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            inputs.NumDescs = count;
            inputs.InstanceDescs = instanceBuffer->GetGPUVirtualAddress();

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
            device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
            if (prebuild.ResultDataMaxSizeInBytes == 0)
                throw std::runtime_error("TLAS prebuild info 无效");

            AccelerationStructure data;
            data.instanceDescs = instanceBuffer;
            data.resource = CreateBuffer(device, prebuild.ResultDataMaxSizeInBytes,
                D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            data.scratch = CreateBuffer(device, prebuild.ScratchDataSizeInBytes,
                D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
            build.Inputs = inputs;
            build.ScratchAccelerationStructureData = data.scratch->GetGPUVirtualAddress();
            build.DestAccelerationStructureData = data.resource->GetGPUVirtualAddress();

            cmdList->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
            InsertUavBarrier(cmdList, data.resource.Get());

            data.gpuAddress = data.resource->GetGPUVirtualAddress();
            return data;
        }
	}

    void AccelerationScene::Build(ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        const MeshLibrary& meshLibrary,
        const std::vector<RenderObject>& objects)
    {
        // DXR 支持检查
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        THROW_IF_FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)));
        if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
            throw std::runtime_error("当前设备不支持 DXR Raytracing Tier 1.1");

        ComPtr<ID3D12Device5> device5;
        THROW_IF_FAILED(device->QueryInterface(IID_PPV_ARGS(&device5)));
        ComPtr<ID3D12GraphicsCommandList4> cmdList4;
        THROW_IF_FAILED(cmdList->QueryInterface(IID_PPV_ARGS(&cmdList4)));

        // 每个 mesh 一个 BLAS
        m_blas.resize(meshLibrary.Count());
        for (uint32_t i = 0; i < meshLibrary.Count(); ++i) {
            const Mesh& mesh = meshLibrary.Get(i);
            m_blas[i] = BuildBLASImpl(device5.Get(), cmdList4.Get(),
                mesh.GetVertexBuffer(), mesh.GetVertexCount(), static_cast<uint32_t>(sizeof(Vertex)),
                mesh.GetIndexBuffer(), mesh.GetIndexCount());
        }

        // 每个物体一个实例
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
        instances.reserve(objects.size());
        for (const RenderObject& obj : objects) {
            const DirectX::XMMATRIX world =
                DirectX::XMMatrixScaling(obj.scale.x, obj.scale.y, obj.scale.z) *
                DirectX::XMMatrixRotationRollPitchYaw(obj.rotation.x, obj.rotation.y, obj.rotation.z) *
                DirectX::XMMatrixTranslation(obj.position.x, obj.position.y, obj.position.z);

            // DXR 的 3x4 是 row-major、列向量约定，等于 DirectXMath 世界矩阵的转置
            DirectX::XMFLOAT4X4 t;
            DirectX::XMStoreFloat4x4(&t, DirectX::XMMatrixTranspose(world));

            D3D12_RAYTRACING_INSTANCE_DESC inst{};
            inst.Transform[0][0] = t._11; inst.Transform[0][1] = t._12; inst.Transform[0][2] = t._13; inst.Transform[0][3] = t._14;
            inst.Transform[1][0] = t._21; inst.Transform[1][1] = t._22; inst.Transform[1][2] = t._23; inst.Transform[1][3] = t._24;
            inst.Transform[2][0] = t._31; inst.Transform[2][1] = t._32; inst.Transform[2][2] = t._33; inst.Transform[2][3] = t._34;

            inst.InstanceID = static_cast<UINT>(instances.size());
            inst.InstanceMask = 0xFF;
            inst.InstanceContributionToHitGroupIndex = 0;
            inst.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
            inst.AccelerationStructure = m_blas[obj.meshId].gpuAddress;

            instances.push_back(inst);
        }

        m_tlas = BuildTLASImpl(device5.Get(), cmdList4.Get(), instances);
    }

    void AccelerationScene::ReleaseBuildScratch()
    {
        for (auto& blas : m_blas) blas.ReleaseScratch();
        m_tlas.ReleaseScratch();
    }

    void AccelerationScene::CreateTLASDescriptor(ID3D12Device* device, GPUDescriptorAllocator& bindless)
    {
        GPUDescriptorHandle tlasSrv = bindless.Allocate();
        m_tlasSrvGpu = tlasSrv.gpuHandle;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.RaytracingAccelerationStructure.Location = m_tlas.gpuAddress;//为 TLAS 创建一个着色器资源视图（SRV）描述符
        //把构建好的 TLAS（场景加速结构）暴露给 GPU 里的 Shader
        device->CreateShaderResourceView(nullptr, &srv, tlasSrv.cpuHandle);
    }

} // namespace rt