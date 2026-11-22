#include "ProbeFixupPass.h"
#include "ProbeGrid.h"
#include "LightingTypes.h"

void ProbeFixupPass::Init(ID3D12RootSignature* rootSignature,
    ID3D12PipelineState* pso,
    ProbeGrid* probeGrid,
    RenderGraph::ResourceId irradianceId,
    RenderGraph::ResourceId distanceId)
{
    m_rootSignature = rootSignature;
    m_pso = pso;
    m_probeGrid = probeGrid;
    m_irradianceId = irradianceId;
    m_distanceId = distanceId;
}

void ProbeFixupPass::Declare(RenderGraph::PassBuilder& builder)
{
    builder.ReadWrite(m_irradianceId, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 2);
    builder.ReadWrite(m_distanceId, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 3);
}

void ProbeFixupPass::Execute(RenderGraph::FrameContext& ctx)
{
    ID3D12GraphicsCommandList* cmdList = ctx.cmdList;
    const ProbeGrid::Desc& grid = m_probeGrid->GetDesc();

    ProbeUpdateParams p{};
    p.countX = grid.countX;
    p.countY = grid.countY;
    p.countZ = grid.countZ;
    p.probeCount = grid.GetProbeCount();
    p.irradianceUav = m_probeGrid->GetUAVIndex();
    p.distanceUav = m_probeGrid->GetDistanceUAVIndex();

    LightingParams lp{};
    lp.objectIndex = ctx.bindings.objectDataBindlessIndex;
    lp.lightCount = ctx.lightCount;

    cmdList->SetComputeRootSignature(m_rootSignature);
    cmdList->SetPipelineState(m_pso);
    cmdList->SetComputeRoot32BitConstants(0, 16, &p, 0);
    cmdList->SetComputeRoot32BitConstants(1, 16, &lp, 0);
    cmdList->SetComputeRootDescriptorTable(2, ctx.bindings.tlasDescriptor);
    cmdList->SetComputeRootConstantBufferView(3, ctx.lightingConstantsAddress);
    cmdList->SetComputeRootShaderResourceView(4, ctx.lightBufferAddress);

    cmdList->Dispatch(grid.GetProbeCount(), 1, 1);
}
