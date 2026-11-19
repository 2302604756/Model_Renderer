#include "ProbeRayShadePass.h"
#include "ProbeGrid.h"
#include "ProbeRayBuffer.h"
#include "LightingTypes.h"

void ProbeRayShadePass::Init(ID3D12RootSignature* rootSignature,
    ID3D12PipelineState* pso,
    ProbeGrid* probeGrid,
    ProbeRayBuffer* rayBuffer,
    RenderGraph::ResourceId rayBufferId)
{
    m_rootSignature = rootSignature;
    m_pso = pso;
    m_probeGrid = probeGrid;
    m_rayBuffer = rayBuffer;
    m_rayBufferId = rayBufferId;

}

void ProbeRayShadePass::Declare(RenderGraph::PassBuilder& builder)
{
    builder.ReadWrite(m_rayBufferId, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void ProbeRayShadePass::Execute(RenderGraph::FrameContext& ctx)
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
    p.frameIndex = ctx.probeFrameIndex;
    p.probeCount = grid.GetProbeCount();
    p.rayCount = ProbeRayBuffer::kRayCount;
    p.rayBufferUav = m_rayBuffer->GetUAVIndex();
    p.objectIndex = ctx.bindings.objectDataBindlessIndex;


    // UAV barrier（写给下一步 texel pass）

    cmdList->SetComputeRootSignature(m_rootSignature);
    cmdList->SetPipelineState(m_pso);

    const uint32_t totalRays = p.probeCount * p.rayCount;

    // 与 ProbeUpdatePass 相同的光照参数
    LightingParams lp{};
    lp.objectIndex = ctx.bindings.objectDataBindlessIndex;
    lp.lightCount = ctx.lightCount;
    cmdList->SetComputeRoot32BitConstants(0, 16, &p, 0);
    cmdList->SetComputeRoot32BitConstants(1, 16, &lp, 0);
    cmdList->SetComputeRootDescriptorTable(2, ctx.bindings.tlasDescriptor);
    cmdList->SetComputeRootConstantBufferView(3, ctx.lightingConstantsAddress);
    cmdList->SetComputeRootShaderResourceView(4, ctx.lightBufferAddress);

    cmdList->Dispatch((totalRays + 63) / 64, 1, 1);

    // 保证 shade pass 能读到 ray 数据
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_rayBuffer->GetResource();
    cmdList->ResourceBarrier(1, &barrier);
}