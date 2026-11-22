#include "LightingPass.h"
#include "RenderTexture.h"
#include "DepthBuffer.h"
#include "LightingTypes.h"

void LightingPass::Init(ID3D12RootSignature* rootSignature, ID3D12PipelineState* pso,
    RenderTexture* albedo, RenderTexture* normal, DepthBuffer* depth,
    RenderGraph::ResourceId albedoId,
    RenderGraph::ResourceId normalId,
    RenderGraph::ResourceId depthId,
    uint32_t depthBindlessIndex,
    uint32_t computeTextureIndex,
    RenderGraph::ResourceId probeFilteredAtlasId,
    const DirectX::XMFLOAT3& gridOrigin,
    float gridSpacing,
    uint32_t gridCountX, uint32_t gridCountY, uint32_t gridCountZ,
    uint32_t probeSrvIndex,
    RenderGraph::ResourceId probeDistanceAtlasId, // 距离矩 atlas 资源
    uint32_t distanceAtlasIndex,
    RenderGraph::ResourceId volumeMaskId,
    uint32_t volumeMaskIndex,
    uint32_t probeMaskIndex,
    RenderGraph::ResourceId probeDataId)
{
    m_rootSignature = rootSignature;
    m_pso = pso;
    m_albedo = albedo;
    m_normal = normal;
    m_depth = depth;
    m_albedoId = albedoId;
    m_normalId = normalId;
    m_depthId = depthId;
    m_depthBindlessIndex = depthBindlessIndex;
    m_computeTextureIndex = computeTextureIndex;
    
    //探针数据
    m_probeFilteredAtlasId = probeFilteredAtlasId;
    m_gridOrigin = gridOrigin;
    m_gridSpacing = gridSpacing;
    m_gridCountX = gridCountX;
    m_gridCountY = gridCountY;
    m_gridCountZ = gridCountZ;
    m_probeSrvIndex = probeSrvIndex;

    m_probeDistanceAtlasId = probeDistanceAtlasId;
    m_probeDataId = probeDataId;
    m_distanceAtlasIndex = distanceAtlasIndex;

    // Mask
    m_volumeMaskId = volumeMaskId;
    m_volumeMaskIndex = volumeMaskIndex;
    m_probeMaskIndex = probeMaskIndex;
}

bool LightingPass::UsesProbeAtlases(uint32_t debugMode) noexcept
{
    return debugMode != 2 && debugMode != 16 && debugMode != 18;
}

void LightingPass::Schedule(RenderGraph& graph, const RenderGraph::FrameContext& frame,
    RenderGraph::ResourceId backbufferId)
{
    m_backbufferId = backbufferId;
    m_tlasId = frame.bindings.tlasResource;
    m_debugMode = frame.debugMode;
    graph.AddPass(*this);
}

void LightingPass::Declare(RenderGraph::PassBuilder& builder)
{
    builder.Read(m_albedoId, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    builder.Read(m_normalId, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    builder.Read(m_depthId, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    builder.Write(m_backbufferId, D3D12_RESOURCE_STATE_RENDER_TARGET);
    builder.Read(m_volumeMaskId, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // 与 LightingPS 的早退分支对应。图集和 TLAS 仅由真正采样它们的模式读取。
    if (m_debugMode == 2)
        return;
    if (!UsesProbeAtlases(m_debugMode)) {
        builder.Read(m_tlasId, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        return;
    }
    if (m_debugMode != 10)
        builder.Read(m_probeFilteredAtlasId, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (m_debugMode != 9)
        builder.Read(m_probeDistanceAtlasId, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (m_debugMode == 9 || m_debugMode == 10)
        return;

    builder.Read(m_probeDataId, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0);
    switch (m_debugMode) {
    case 8:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
    case 17:
        break;
    default:
        builder.Read(m_tlasId, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        break;
    }
}

void LightingPass::Execute(RenderGraph::FrameContext& ctx)
{
    ID3D12GraphicsCommandList* cmdList = ctx.cmdList;

    cmdList->OMSetRenderTargets(1, &ctx.backbufferRtv, FALSE, nullptr);

    const float clearColor[4] = { 0.1f, 0.1f, 0.15f, 1.0f };
    cmdList->ClearRenderTargetView(ctx.backbufferRtv, clearColor, 0, nullptr);

    cmdList->SetGraphicsRootSignature(m_rootSignature);
    cmdList->SetPipelineState(m_pso);
    cmdList->SetGraphicsRootConstantBufferView(1, ctx.frameConstantsAddress);

    LightingParams lp{};
    lp.albedoIndex = m_albedo->GetBindlessIndex();
    lp.normalIndex = m_normal->GetBindlessIndex();
    lp.depthIndex = m_depthBindlessIndex;
    lp.debugMode = ctx.debugMode;
    lp.objectIndex = ctx.bindings.objectDataBindlessIndex;
    lp.lightCount = ctx.lightCount;
    lp.computeTextureIndex = m_computeTextureIndex;
    lp.gridOriginSpacing = { m_gridOrigin.x, m_gridOrigin.y, m_gridOrigin.z, m_gridSpacing };
    lp.gridCountX = m_gridCountX;
    lp.gridCountY = m_gridCountY;
    lp.gridCountZ = m_gridCountZ;
    lp.probeSrvIndex = m_probeSrvIndex;
    lp.distanceAtlasIndex = m_distanceAtlasIndex;

    cmdList->SetGraphicsRoot32BitConstants(2, 16, &lp, 0);
    cmdList->SetGraphicsRootConstantBufferView(4, ctx.lightingConstantsAddress);
    cmdList->SetGraphicsRootShaderResourceView(5, ctx.lightBufferAddress);
    cmdList->SetGraphicsRootDescriptorTable(3, ctx.bindings.tlasDescriptor);

    cmdList->DrawInstanced(3, 1, 0, 0);
}
