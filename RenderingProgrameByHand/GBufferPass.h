#pragma once
#include "Pass.h"
#include <d3d12.h>

class RenderTexture;
class DepthBuffer;
class Scene;
class MeshLibrary;
struct ID3D12PipelineState;

class GBufferPass : public RenderPass {
public:
    void Init(ID3D12PipelineState* pso,
        RenderTexture* albedo, RenderTexture* normal, DepthBuffer* depth,
        const Scene* scene, const MeshLibrary* meshLibrary,
        RenderGraph::ResourceId albedoId,
        RenderGraph::ResourceId normalId,
        RenderGraph::ResourceId depthId);

    const char* GetName() const override { return "GBuffer"; }
    void Declare(RenderGraph::PassBuilder& builder) override;
    void Execute(RenderGraph::FrameContext& ctx) override;

private:
    ID3D12PipelineState* m_pso = nullptr;
    RenderTexture* m_albedo = nullptr;
    RenderTexture* m_normal = nullptr;
    DepthBuffer* m_depth = nullptr;
    const Scene* m_scene = nullptr;
    const MeshLibrary* m_meshLibrary = nullptr;

    RenderGraph::ResourceId m_albedoId = RenderGraph::kInvalidId;
    RenderGraph::ResourceId m_normalId = RenderGraph::kInvalidId;
    RenderGraph::ResourceId m_depthId = RenderGraph::kInvalidId;
};