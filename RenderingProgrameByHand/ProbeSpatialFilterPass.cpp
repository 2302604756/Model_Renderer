#include "ProbeSpatialFilterPass.h"
#include "ProbeGrid.h"
#include "LightingTypes.h"
#include "ProbeTypes.h"

void ProbeSpatialFilterPass::Init(
    ID3D12RootSignature* rootSignature,
    ID3D12PipelineState* pso,
    ProbeGrid* probeGrid,
    RenderGraph::ResourceId rawAtlasId,
    RenderGraph::ResourceId filteredAtlasId,
    RenderGraph::ResourceId distanceAtlasId)
{
    m_rootSignature = rootSignature;
    m_pso = pso;
    m_probeGrid = probeGrid;
    m_rawAtlasId = rawAtlasId;
    m_filteredAtlasId = filteredAtlasId;
    m_distanceAtlasId = distanceAtlasId;
}

void ProbeSpatialFilterPass::Declare(RenderGraph::PassBuilder& builder)
{
    builder.Read(
        m_rawAtlasId,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    builder.Read(
        m_distanceAtlasId,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    builder.Write(
        m_filteredAtlasId,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void ProbeSpatialFilterPass::Execute(RenderGraph::FrameContext& ctx)
{
    ID3D12GraphicsCommandList* cmdList = ctx.cmdList;
    const ProbeGrid::Desc& grid = m_probeGrid->GetDesc();

    ProbeUpdateParams p{};
    p.countX = grid.countX;
    p.countY = grid.countY;
    p.countZ = grid.countZ;
    p.probeCount = grid.GetProbeCount();
    p.irradianceUav = m_probeGrid->GetFilteredUAVIndex();

    LightingParams lp{};
    lp.probeSrvIndex = m_probeGrid->GetSRVIndex(); // raw input
    lp.distanceAtlasIndex = m_probeGrid->GetDistanceSRVIndex();
    lp.gridOriginSpacing = {
        grid.origin.x,
        grid.origin.y,
        grid.origin.z,
        grid.spacing
    };
    lp.gridCountX = grid.countX;
    lp.gridCountY = grid.countY;
    lp.gridCountZ = grid.countZ;

    cmdList->SetComputeRootSignature(m_rootSignature);
    cmdList->SetPipelineState(m_pso);
    cmdList->SetComputeRoot32BitConstants(0, 16, &p, 0);
    cmdList->SetComputeRoot32BitConstants(1, 16, &lp, 0);
    cmdList->SetComputeRootConstantBufferView(
        3,
        ctx.lightingConstantsAddress);

    cmdList->Dispatch(grid.GetProbeCount(), 1, 1);
}
