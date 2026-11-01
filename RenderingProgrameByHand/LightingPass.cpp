#include "LightingPass.h"
#include "RenderTexture.h"
#include "DepthBuffer.h"
#include "LightingTypes.h"

void LightingPass::Init(ID3D12PipelineState* pso,
    RenderTexture* albedo, RenderTexture* normal, DepthBuffer* depth,
    RenderGraph::ResourceId albedoId,
    RenderGraph::ResourceId normalId,
    RenderGraph::ResourceId depthId,
    uint32_t depthBindlessIndex,
    uint32_t objectDataBindlessIndex,
    D3D12_GPU_DESCRIPTOR_HANDLE tlasDescriptor,
    uint32_t computeTextureIndex,
    RenderGraph::ResourceId probeGridId,
    const DirectX::XMFLOAT3& gridOrigin,
    float gridSpacing,
    uint32_t gridCountX, uint32_t gridCountY, uint32_t gridCountZ,
    uint32_t probeSrvIndex,
    RenderGraph::ResourceId probeDistanceAtlasId, // 距离矩 atlas 资源
    uint32_t distanceAtlasIndex)
{
    m_pso = pso;
    m_albedo = albedo;
    m_normal = normal;
    m_depth = depth;
    m_albedoId = albedoId;
    m_normalId = normalId;
    m_depthId = depthId;
    m_depthBindlessIndex = depthBindlessIndex;
    m_objectDataBindlessIndex = objectDataBindlessIndex;
    m_tlasDescriptor = tlasDescriptor;
    m_computeTextureIndex = computeTextureIndex;
    
    //探针数据
    m_probeGridId = probeGridId;
    m_gridOrigin = gridOrigin;
    m_gridSpacing = gridSpacing;
    m_gridCountX = gridCountX;
    m_gridCountY = gridCountY;
    m_gridCountZ = gridCountZ;
    m_probeSrvIndex = probeSrvIndex;

    m_probeDistanceAtlasId = probeDistanceAtlasId;
    m_distanceAtlasIndex = distanceAtlasIndex;
}

void LightingPass::Declare(RenderGraph::PassBuilder& builder)
{
    builder.Read(m_albedoId, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    builder.Read(m_normalId, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    builder.Read(m_depthId, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    builder.Write(m_backbufferId, D3D12_RESOURCE_STATE_RENDER_TARGET);
    builder.Read(m_probeGridId, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    builder.Read(m_probeDistanceAtlasId, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

}

void LightingPass::Execute(RenderGraph::FrameContext& ctx)
{
    ID3D12GraphicsCommandList* cmdList = ctx.cmdList;

    cmdList->OMSetRenderTargets(1, &ctx.backbufferRtv, FALSE, nullptr);

    const float clearColor[4] = { 0.1f, 0.1f, 0.15f, 1.0f };
    cmdList->ClearRenderTargetView(ctx.backbufferRtv, clearColor, 0, nullptr);

    cmdList->SetPipelineState(m_pso);

    LightingParams lp{};
    lp.albedoIndex = m_albedo->GetBindlessIndex();
    lp.normalIndex = m_normal->GetBindlessIndex();
    lp.depthIndex = m_depthBindlessIndex;
    lp.debugMode = ctx.debugMode;
    lp.objectIndex = m_objectDataBindlessIndex;
    lp.lightCount = ctx.lightCount;
    lp.computeTextureIndex = m_computeTextureIndex;
    lp.gridOriginSpacing = { m_gridOrigin.x, m_gridOrigin.y, m_gridOrigin.z, m_gridSpacing };
    lp.gridCountX = m_gridCountX;
    lp.gridCountY = m_gridCountY;
    lp.gridCountZ = m_gridCountZ;
    lp.probeSrvIndex = m_probeSrvIndex;
    lp.distanceAtlasIndex = m_distanceAtlasIndex;

    cmdList->SetGraphicsRoot32BitConstants(2, 16, &lp, 0);   // 12 → 16
    cmdList->SetGraphicsRootConstantBufferView(4, ctx.lightingConstantsAddress);
    cmdList->SetGraphicsRootShaderResourceView(5, ctx.lightBufferAddress);
    cmdList->SetGraphicsRootDescriptorTable(3, m_tlasDescriptor);

    cmdList->DrawInstanced(3, 1, 0, 0);
}