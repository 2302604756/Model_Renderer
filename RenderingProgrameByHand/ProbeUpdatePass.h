#pragma once
#include "Pass.h"
#include "ProbeTypes.h"
#include <d3d12.h>

class ProbeGrid;

class ProbeUpdatePass : public RenderPass {
public:
    void Init(ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pso,
        ProbeGrid* probeGrid,
        RenderGraph::ResourceId probeGridId,
        uint32_t objectDataBindlessIndex,
        D3D12_GPU_DESCRIPTOR_HANDLE tlasDescriptor);

    const char* GetName() const override { return "ProbeUpdate"; }
    void Declare(RenderGraph::PassBuilder& builder) override;
    void Execute(RenderGraph::FrameContext& ctx) override;

private:
    ID3D12RootSignature* m_rootSignature = nullptr;
    ID3D12PipelineState* m_pso = nullptr;
    ProbeGrid* m_probeGrid = nullptr;
    RenderGraph::ResourceId m_probeGridId = RenderGraph::kInvalidId;
    uint32_t                m_objectDataBindlessIndex = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE m_tlasDescriptor{};
    uint32_t                m_frameIndex = 0;   // 单调帧号（累积用）
};