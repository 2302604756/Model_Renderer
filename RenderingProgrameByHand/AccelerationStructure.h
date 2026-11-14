#pragma once
#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <cstdint>
#include "SceneTypes.h"
#include "MeshLibrary.h"
#include "BindlessDescriptorHeap.h"

using Microsoft::WRL::ComPtr;

namespace rt {

    // 一个 AS 本体 + 构建期临时资源（GPU 完成后调用 ReleaseScratch）
    struct AccelerationStructure {
        ComPtr<ID3D12Resource>    resource;      // AS 本体（保留）
        ComPtr<ID3D12Resource>    scratch;       // 构建用 scratch（可释放）
        ComPtr<ID3D12Resource>    instanceDescs; // 仅 TLAS：实例描述缓冲
        D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0;

        void ReleaseScratch() {
            scratch.Reset();
            instanceDescs.Reset();
        }
    };

    // 场景级 BLAS / TLAS：构建、临时资源生命周期、TLAS 描述符
    class AccelerationScene {
    public:
        static constexpr uint32_t kFrameCount = 3;
        // 把 BLAS/TLAS 构建命令录制进 cmdList；执行和等待由调用方负责
        void Build(ID3D12Device* device,
            ID3D12GraphicsCommandList* cmdList,
            const MeshLibrary& meshLibrary,
            const std::vector<RenderObject>& objects);

        // GPU 完成构建后调用
        void ReleaseBuildScratch();
        void CreateTLASDescriptor(ID3D12Device* device, GPUDescriptorAllocator& bindless);

        struct TLASFrame {
            AccelerationStructure         tlas;
            D3D12_GPU_DESCRIPTOR_HANDLE   srv{};
        };

        //获取当前帧 TLAS
        D3D12_GPU_DESCRIPTOR_HANDLE GetTLASDescriptor(uint32_t frameIndex) const{  return m_tlasFrames[frameIndex % 3].srv; }

        // 每帧更新 TLAS（实例矩阵变化），BLAS 复用
        void UpdateTLAS(ID3D12GraphicsCommandList* cmdList,
            const std::vector<RenderObject>& objects,
            uint32_t frameIndex);

    private:
        std::vector<AccelerationStructure> m_blas;//blas和tlas本质是一样的？只是使用方式既操作方法不同
        TLASFrame                          m_tlasFrames[3]; // 后续多帧资源
        // 初始实例数，阶段 2 固定 7 个物体
        uint32_t                           m_instanceCapacity = 0;
    };

} // namespace rt