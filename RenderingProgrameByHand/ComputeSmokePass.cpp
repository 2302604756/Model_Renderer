#include "ComputeSmokePass.h"
#include "RenderTexture.h"

void ComputeSmokePass::Init(ID3D12RootSignature* rootSignature,
    ID3D12PipelineState* pso,
    RenderTexture* target,
    RenderGraph::ResourceId targetId,
    uint32_t width, uint32_t height)
{
    m_rootSignature = rootSignature;
    m_pso = pso;
    m_target = target;
    m_targetId = targetId;
    m_width = width;
    m_height = height;
}

void ComputeSmokePass::Declare(RenderGraph::PassBuilder& builder)
{
    builder.ReadWrite(m_targetId, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void ComputeSmokePass::Execute(RenderGraph::FrameContext& ctx)
{
    ID3D12GraphicsCommandList* cmdList = ctx.cmdList;

    struct ComputeParams {
        uint32_t outUavIndex;
        uint32_t width;
        uint32_t height;
        uint32_t frameIndex;
        uint32_t padding[4];
    } params{};

    params.outUavIndex = m_target->GetUAVBindlessIndex();
    params.width = m_width;
    params.height = m_height;
    params.frameIndex = ctx.frameIndex;

    cmdList->SetComputeRootSignature(m_rootSignature);
    cmdList->SetPipelineState(m_pso);
    cmdList->SetComputeRoot32BitConstants(0, 8, &params, 0);
    cmdList->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);

    // UAV barrier：保证后续 pass 能读到写入结果
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_target->GetResource();
    cmdList->ResourceBarrier(1, &barrier);
}