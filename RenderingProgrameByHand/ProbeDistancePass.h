#pragma once
#include "Pass.h"
#include "ProbeTypes.h"
#include "ProbeRayBuffer.h"
#include <d3d12.h>

class ProbeGrid;

// 距离矩更新 pass：16×16 texel / 探针
class ProbeDistancePass : public RenderPass {
public:
    void Init(ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pso,
        ProbeGrid* probeGrid,
        ProbeRayBuffer* rayBuffer,
        RenderGraph::ResourceId rayBufferId,
        RenderGraph::ResourceId distanceAtlasId);

    const char* GetName() const override { return "ProbeDistance"; }
    void Declare(RenderGraph::PassBuilder& builder) override;
    void Execute(RenderGraph::FrameContext& ctx) override;

private:
    ID3D12RootSignature* m_rootSignature = nullptr;
    ID3D12PipelineState* m_pso = nullptr;
    ProbeGrid* m_probeGrid = nullptr;
    RenderGraph::ResourceId m_distanceAtlasId = RenderGraph::kInvalidId;
    ProbeRayBuffer* m_rayBuffer = nullptr;
    RenderGraph::ResourceId m_rayBufferId = RenderGraph::kInvalidId;
};