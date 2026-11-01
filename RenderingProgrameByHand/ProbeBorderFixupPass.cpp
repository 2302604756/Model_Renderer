#include "ProbeBorderFixupPass.h"

void ProbeBorderFixupPass::Init(ID3D12RootSignature* rootSignature,
    ID3D12PipelineState* pso,
    RenderGraph::ResourceId atlasId,
    ID3D12Resource* atlasResource,
    uint32_t countX, uint32_t countY, uint32_t countZ,
    uint32_t uavIndex,
    uint32_t stride,
    uint32_t atlasWidth,
    uint32_t atlasHeight)
{
    m_rootSignature = rootSignature;
    m_pso = pso;
    m_atlasId = atlasId;
    m_atlasResource = atlasResource;
    m_countX = countX;
    m_countY = countY;
    m_countZ = countZ;
    m_uavIndex = uavIndex;
    m_stride = stride;
    m_atlasWidth = atlasWidth;
    m_atlasHeight = atlasHeight;
}

void ProbeBorderFixupPass::Declare(RenderGraph::PassBuilder& builder)
{
    builder.ReadWrite(m_atlasId, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void ProbeBorderFixupPass::Execute(RenderGraph::FrameContext& ctx)
{
    ID3D12GraphicsCommandList* cmdList = ctx.cmdList;

    // 只填 border fixup 需要的字段
    ProbeUpdateParams p{};
    p.countX = m_countX;
    p.countY = m_countY;
    p.countZ = m_countZ;
    p.borderTargetUav = m_uavIndex;
    p.borderTileStride = m_stride;

    cmdList->SetComputeRootSignature(m_rootSignature);
    cmdList->SetPipelineState(m_pso);
    cmdList->SetComputeRoot32BitConstants(0, 16, &p, 0);

    // 覆盖整张 atlas
    cmdList->Dispatch((m_atlasWidth + 7) / 8, (m_atlasHeight + 7) / 8, 1);

    // 确保后续 pass 能读到 gutter
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_atlasResource;
    cmdList->ResourceBarrier(1, &barrier);
}