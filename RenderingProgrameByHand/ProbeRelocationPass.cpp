#include "ProbeRelocationPass.h"
#include "ProbeGrid.h"
#include "ProbeRayBuffer.h"

void ProbeRelocationPass::Init(ID3D12RootSignature* rootSignature,
    ID3D12PipelineState* pso,
    ProbeGrid* probeGrid,
    ProbeRayBuffer* rayBuffer,
    RenderGraph::ResourceId rayBufferId,
    RenderGraph::ResourceId probeDataId)
{
    m_rootSignature = rootSignature;
    m_pso = pso;
    m_probeGrid = probeGrid;
    m_rayBuffer = rayBuffer;
    m_rayBufferId = rayBufferId;
    m_probeDataId = probeDataId;
}

void ProbeRelocationPass::Declare(RenderGraph::PassBuilder& builder)
{
    // 只读 ray buffer，但通过 UAV 描述符访问，状态保持 UNORDERED_ACCESS
    builder.Read(m_rayBufferId, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    builder.ReadWrite(m_probeDataId, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void ProbeRelocationPass::Execute(RenderGraph::FrameContext& ctx)
{
    ID3D12GraphicsCommandList* cmdList = ctx.cmdList;
    // 动态场景每帧都要更新 offset；论文中的“收敛后冻结”只适用于静态场景。
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
    p.rayBufferUav = m_rayBuffer->GetUAVIndex();

    cmdList->SetComputeRootSignature(m_rootSignature);
    cmdList->SetPipelineState(m_pso);
    cmdList->SetComputeRoot32BitConstants(0, 16, &p, 0);                        // b4
    cmdList->SetComputeRootDescriptorTable(2, ctx.bindings.tlasDescriptor);     // t3（本 pass 不读，保持与其他 pass 一致）
    cmdList->SetComputeRootConstantBufferView(3, ctx.lightingConstantsAddress); // b3：probeDataUavIndex
    cmdList->SetComputeRootShaderResourceView(4, ctx.lightBufferAddress);       // t5（本 pass 不读）

    // 一个线程 = 一个探针（605 个探针 → 10 个 group）
    cmdList->Dispatch((p.probeCount + 63) / 64, 1, 1);
}
