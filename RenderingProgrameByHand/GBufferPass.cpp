#include "GBufferPass.h"
#include "RenderTexture.h"
#include "DepthBuffer.h"
#include "Scene.h"
#include "MeshLibrary.h"
#include "ObjectConstants.h"
#include <DirectXMath.h>

void GBufferPass::Init(ID3D12PipelineState* pso,
    RenderTexture* albedo, RenderTexture* normal, DepthBuffer* depth,
    const Scene* scene, const MeshLibrary* meshLibrary,
    RenderGraph::ResourceId albedoId,
    RenderGraph::ResourceId normalId,
    RenderGraph::ResourceId depthId)
{
    m_pso = pso;
    m_albedo = albedo;
    m_normal = normal;
    m_depth = depth;
    m_scene = scene;
    m_meshLibrary = meshLibrary;
    m_albedoId = albedoId;
    m_normalId = normalId;
    m_depthId = depthId;
}

void GBufferPass::Declare(RenderGraph::PassBuilder& builder)
{
    builder.Write(m_albedoId, D3D12_RESOURCE_STATE_RENDER_TARGET);
    builder.Write(m_normalId, D3D12_RESOURCE_STATE_RENDER_TARGET);
    builder.Write(m_depthId, D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

void GBufferPass::Execute(RenderGraph::FrameContext& ctx)
{
    ID3D12GraphicsCommandList* cmdList = ctx.cmdList;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {
        m_albedo->GetRTV(), m_normal->GetRTV() };
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_depth->GetDSV();
    cmdList->OMSetRenderTargets(2, rtvs, FALSE, &dsv);

    const float clearAlbedo[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const float clearNormal[4] = { 0.5f, 0.5f, 1.0f, 1.0f };
    cmdList->ClearRenderTargetView(rtvs[0], clearAlbedo, 0, nullptr);
    cmdList->ClearRenderTargetView(rtvs[1], clearNormal, 0, nullptr);
    m_depth->Clear(cmdList);

    cmdList->SetPipelineState(m_pso);
    cmdList->SetGraphicsRootConstantBufferView(1, ctx.frameConstantsAddress);

    for (const RenderObject& obj : m_scene->GetObjects())
    {
        const DirectX::XMMATRIX world =
            DirectX::XMMatrixScaling(obj.scale.x, obj.scale.y, obj.scale.z) *
            DirectX::XMMatrixRotationRollPitchYaw(obj.rotation.x, obj.rotation.y, obj.rotation.z) *
            DirectX::XMMatrixTranslation(obj.position.x, obj.position.y, obj.position.z);

        PerObjectConstants objectData{};
        DirectX::XMStoreFloat4x4(&objectData.world, DirectX::XMMatrixTranspose(world));
        DirectX::XMStoreFloat4x4(&objectData.normalMatrix, DirectX::XMMatrixInverse(nullptr, world));

        const Mesh& mesh = m_meshLibrary->Get(obj.meshId);
        objectData.vbIndex = mesh.GetVBBindlessIndex();
        objectData.textureIndex = obj.textureIndex;
        objectData.albedo = obj.albedo;

        cmdList->SetGraphicsRoot32BitConstants(0, 40, &objectData, 0);
        cmdList->IASetIndexBuffer(&mesh.GetIBV());
        cmdList->DrawIndexedInstanced(mesh.GetIndexCount(), 1, 0, 0, 0);
    }
}
