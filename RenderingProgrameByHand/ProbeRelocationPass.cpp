#include "ProbeRelocationPass.h"
#include "ProbeGrid.h"
#include "ProbeRayBuffer.h"

void ProbeRelocationPass::Init(ID3D12RootSignature* rootSignature,
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

void ProbeRelocationPass::Declare(RenderGraph::PassBuilder& builder)
{
    // 只读 ray buffer，但通过 UAV 描述符访问，状态保持 UNORDERED_ACCESS
    builder.ReadWrite(m_rayBufferId, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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

    // 本帧 Lighting 已经读过旧 offset，写之前先等这些读完成
    D3D12_RESOURCE_BARRIER preBarrier{};
    preBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    preBarrier.UAV.pResource = m_probeGrid->GetProbeDataResource();
    cmdList->ResourceBarrier(1, &preBarrier);

    cmdList->SetComputeRootSignature(m_rootSignature);
    cmdList->SetPipelineState(m_pso);
    cmdList->SetComputeRoot32BitConstants(0, 16, &p, 0);                        // b4
    cmdList->SetComputeRootDescriptorTable(2, ctx.bindings.tlasDescriptor);     // t3（本 pass 不读，保持与其他 pass 一致）
    cmdList->SetComputeRootConstantBufferView(3, ctx.lightingConstantsAddress); // b3：probeDataUavIndex
    cmdList->SetComputeRootShaderResourceView(4, ctx.lightBufferAddress);       // t5（本 pass 不读）

    // 一个线程 = 一个探针（605 个探针 → 10 个 group）
    cmdList->Dispatch((p.probeCount + 63) / 64, 1, 1);

    // 写完 offset 后，下一帧 ProbeRay/Distance/Lighting 才能安全读取；
    // 同时本帧 relocation 读过的 ray buffer，下一帧 ProbeRay 要覆盖写，需要 UAV barrier。
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = m_probeGrid->GetProbeDataResource();
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].UAV.pResource = m_rayBuffer->GetResource();
    cmdList->ResourceBarrier(2, barriers);
}