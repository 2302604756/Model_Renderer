#pragma once
#include "Pass.h"
#include "ProbeTypes.h"
#include <d3d12.h>

class ProbeGrid;
class ProbeRayBuffer;

class ProbeRayPass : public RenderPass {
public:
    void Init(ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pso,
        ProbeGrid* probeGrid,
        ProbeRayBuffer* rayBuffer,
        RenderGraph::ResourceId rayBufferId);

    const char* GetName() const override { return "ProbeRay"; }
    void Declare(RenderGraph::PassBuilder& builder) override;
    void Execute(RenderGraph::FrameContext& ctx) override;

private:
    ID3D12RootSignature*        m_rootSignature = nullptr;
    ID3D12PipelineState*        m_pso = nullptr;
    ProbeGrid*                  m_probeGrid = nullptr;
    ProbeRayBuffer*             m_rayBuffer = nullptr;
    RenderGraph::ResourceId     m_rayBufferId = RenderGraph::kInvalidId;
    uint32_t                    m_frameIndex = 0;
};