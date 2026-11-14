#pragma once
#include "Pass.h"
#include "ProbeTypes.h"
#include <d3d12.h>

class ProbeGrid;

class ProbeFixupPass : public RenderPass {
public:
    void Init(ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pso,
        ProbeGrid* probeGrid,
        RenderGraph::ResourceId irradianceId,
        RenderGraph::ResourceId distanceId);

    const char* GetName() const override { return "ProbeFixup"; }
    void Declare(RenderGraph::PassBuilder& builder) override;
    void Execute(RenderGraph::FrameContext& ctx) override;

private:
    ID3D12RootSignature* m_rootSignature = nullptr;
    ID3D12PipelineState* m_pso = nullptr;
    ProbeGrid* m_probeGrid = nullptr;

    RenderGraph::ResourceId m_irradianceId = RenderGraph::kInvalidId;
    RenderGraph::ResourceId m_distanceId = RenderGraph::kInvalidId;
};