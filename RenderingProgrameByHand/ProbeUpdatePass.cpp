#include "ProbeUpdatePass.h"
#include "ProbeGrid.h"
#include "LightingTypes.h"

void ProbeUpdatePass::Init(ID3D12RootSignature* rootSignature,
    ID3D12PipelineState* pso,
    ProbeGrid* probeGrid,
    ProbeRayBuffer* rayBuffer,
    RenderGraph::ResourceId probeGridId,
    RenderGraph::ResourceId rayBufferId)
{
    m_rootSignature = rootSignature;
    m_pso = pso;
    m_probeGrid = probeGrid;
    m_probeGridId = probeGridId;
    m_rayBuffer = rayBuffer;
    m_rayBufferId = rayBufferId;
}

void ProbeUpdatePass::Declare(RenderGraph::PassBuilder& builder)
{
    builder.ReadWrite(m_probeGridId, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    builder.ReadWrite(m_rayBufferId, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); 
}

void ProbeUpdatePass::Execute(RenderGraph::FrameContext& ctx)
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
    p.rayCount = ProbeRayBuffer::kRayCount;
    p.objectIndex = ctx.bindings.objectDataBindlessIndex;
    p.irradianceUav = m_probeGrid->GetUAVIndex();
    p.rayBufferUav = m_rayBuffer->GetUAVIndex();


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

    // 每个 group = 一个探针（8×8 线程 = 64 个 octahedral texel）
    // Dispatch分发线程的单位是我们在Shader中填写的[8,8,1],指在x方向上分发count个8*8*1
    cmdList->Dispatch(grid.GetProbeCount(), 1, 1);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_probeGrid->GetResource();
    cmdList->ResourceBarrier(1, &barrier);
}