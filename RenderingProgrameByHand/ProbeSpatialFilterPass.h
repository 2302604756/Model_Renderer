#pragma once
#include "Pass.h"
#include <d3d12.h>

class ProbeGrid;

class ProbeSpatialFilterPass : public RenderPass {
public:
    void Init(
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pso,
        ProbeGrid* probeGrid,
        RenderGraph::ResourceId rawAtlasId,
        RenderGraph::ResourceId filteredAtlasId,
        RenderGraph::ResourceId distanceAtlasId);

    const char* GetName() const override {
        return "ProbeSpatialFilter";
    }

    void Declare(RenderGraph::PassBuilder& builder) override;
    void Execute(RenderGraph::FrameContext& ctx) override;

private:
    ID3D12RootSignature* m_rootSignature = nullptr;
    ID3D12PipelineState* m_pso = nullptr;
    ProbeGrid* m_probeGrid = nullptr;

    RenderGraph::ResourceId m_rawAtlasId = RenderGraph::kInvalidId;
    RenderGraph::ResourceId m_filteredAtlasId = RenderGraph::kInvalidId;
    RenderGraph::ResourceId m_distanceAtlasId = RenderGraph::kInvalidId;
};
