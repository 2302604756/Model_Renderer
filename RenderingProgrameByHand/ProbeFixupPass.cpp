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
    builder.ReadWrite(m_irradianceId, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    builder.ReadWrite(m_distanceId, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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

    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = m_probeGrid->GetResource();
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].UAV.pResource = m_probeGrid->GetDistanceResource();
    cmdList->ResourceBarrier(2, barriers);
}