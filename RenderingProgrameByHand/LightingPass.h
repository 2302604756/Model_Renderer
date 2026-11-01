#pragma once
#include "Pass.h"
#include <d3d12.h>
#include <DirectXMath.h>

class RenderTexture;
class DepthBuffer;
struct ID3D12PipelineState;

class LightingPass : public RenderPass {
public:
    void Init(ID3D12PipelineState* pso,
        RenderTexture* albedo, RenderTexture* normal, DepthBuffer* depth,
        RenderGraph::ResourceId albedoId,
        RenderGraph::ResourceId normalId,
        RenderGraph::ResourceId depthId,
        uint32_t depthBindlessIndex,
        uint32_t objectDataBindlessIndex,
        D3D12_GPU_DESCRIPTOR_HANDLE tlasDescriptor,
        uint32_t computeTextureIndex,
        RenderGraph::ResourceId probeGridId,
        const DirectX::XMFLOAT3& gridOrigin,
        float gridSpacing,
        uint32_t gridCountX, uint32_t gridCountY, uint32_t gridCountZ,
        uint32_t probeSrvIndex,
        RenderGraph::ResourceId probeDistanceAtlasId, // 距离矩 atlas 资源
        uint32_t distanceAtlasIndex);                 // 距离矩 atlas SRV

    // 每帧由 App 设置（backbuffer 在 3 张图之间轮换）
    void SetBackbuffer(RenderGraph::ResourceId backbufferId) { m_backbufferId = backbufferId; }

    const char* GetName() const override { return "Lighting"; }
    void Declare(RenderGraph::PassBuilder& builder) override;
    void Execute(RenderGraph::FrameContext& ctx) override;

private:
    ID3D12PipelineState* m_pso = nullptr;
    RenderTexture* m_albedo = nullptr;
    RenderTexture* m_normal = nullptr;
    DepthBuffer* m_depth = nullptr;

    RenderGraph::ResourceId m_albedoId = RenderGraph::kInvalidId;
    RenderGraph::ResourceId m_normalId = RenderGraph::kInvalidId;
    RenderGraph::ResourceId m_depthId = RenderGraph::kInvalidId;
    RenderGraph::ResourceId m_backbufferId = RenderGraph::kInvalidId;

    uint32_t m_depthBindlessIndex = 0;
    uint32_t m_objectDataBindlessIndex = 0;
    uint32_t m_computeTextureIndex = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE m_tlasDescriptor{};

    //探针pass
    RenderGraph::ResourceId m_probeGridId = RenderGraph::kInvalidId;
    DirectX::XMFLOAT3       m_gridOrigin{};
    float                   m_gridSpacing = 1.0f;
    uint32_t                m_gridCountX = 0, m_gridCountY = 0, m_gridCountZ = 0;
    uint32_t                m_probeSrvIndex = 0;
    RenderGraph::ResourceId m_probeDistanceAtlasId = RenderGraph::kInvalidId;
    uint32_t                m_distanceAtlasIndex = 0;
};