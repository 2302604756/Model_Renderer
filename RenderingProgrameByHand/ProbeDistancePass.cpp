#include "ProbeDistancePass.h"
#include "ProbeGrid.h"

void ProbeDistancePass::Init(ID3D12RootSignature* rootSignature,
    ID3D12PipelineState* pso,
    ProbeGrid* probeGrid,
    RenderGraph::ResourceId distanceAtlasId,
    D3D12_GPU_DESCRIPTOR_HANDLE tlasDescriptor)
{
    m_rootSignature = rootSignature;
    m_pso = pso;
    m_probeGrid = probeGrid;
    m_distanceAtlasId = distanceAtlasId;
    m_tlasDescriptor = tlasDescriptor;
}

void ProbeDistancePass::Declare(RenderGraph::PassBuilder& builder)
{
    builder.ReadWrite(m_distanceAtlasId, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void ProbeDistancePass::Execute(RenderGraph::FrameContext& ctx)
{
    ID3D12GraphicsCommandList* cmdList = ctx.cmdList;
    const ProbeGrid::Desc& grid = m_probeGrid->GetDesc();

    ProbeUpdateParams p{};
    p.originX = grid.origin.x;
    p.originY = grid.origin.y;
    p.originZ = grid.origin.z;
    p.spacing = grid.spacing;
    p.countX = grid.countX;
    p.countY = grid.countY;
    p.countZ = grid.countZ;
    p.frameIndex = m_frameIndex++;
    p.probeCount = grid.GetProbeCount();
    p.distanceUav = m_probeGrid->GetDistanceUAVIndex();

    cmdList->SetComputeRootSignature(m_rootSignature);
    cmdList->SetPipelineState(m_pso);
    cmdList->SetComputeRoot32BitConstants(0, 16, &p, 0);
    cmdList->SetComputeRootDescriptorTable(2, m_tlasDescriptor); // t3

    // 每个 group = 一个探针（16×16 线程）
    cmdList->Dispatch(grid.GetProbeCount(), 1, 1);

    // UAV barrier：后续 pass 才能读到
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_probeGrid->GetDistanceResource();
    cmdList->ResourceBarrier(1, &barrier);
}