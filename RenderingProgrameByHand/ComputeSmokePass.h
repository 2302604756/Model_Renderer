#pragma once
#include "Pass.h"
#include <d3d12.h>

class RenderTexture;

class ComputeSmokePass : public RenderPass {
public:
    void Init(ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pso,
        RenderTexture* target,
        RenderGraph::ResourceId targetId,
        uint32_t width, uint32_t height);

    const char* GetName() const override { return "ComputeSmoke"; }
    void Declare(RenderGraph::PassBuilder& builder) override;
    void Execute(RenderGraph::FrameContext& ctx) override;

private:
    ID3D12RootSignature* m_rootSignature = nullptr;
    ID3D12PipelineState* m_pso = nullptr;
    RenderTexture* m_target = nullptr;
    RenderGraph::ResourceId m_targetId = RenderGraph::kInvalidId;
    uint32_t                m_width = 1920;
    uint32_t                m_height = 1080;
};