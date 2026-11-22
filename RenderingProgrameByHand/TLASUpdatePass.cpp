#include "TLASUpdatePass.h"
#include "AccelerationStructure.h"
#include "Scene.h"

void TLASUpdatePass::Init(rt::AccelerationScene* rtScene, const Scene* scene)
{
    m_rtScene = rtScene;
    m_scene = scene;
}

void TLASUpdatePass::Schedule(RenderGraph& graph, RenderGraph::ResourceId tlas)
{
    m_tlas = tlas;
    graph.AddPass(*this);
}

void TLASUpdatePass::Declare(RenderGraph::PassBuilder& builder)
{
    builder.Write(m_tlas, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
}

void TLASUpdatePass::Execute(RenderGraph::FrameContext& ctx)
{
    m_rtScene->UpdateTLAS(ctx.cmdList, m_scene->GetObjects(), ctx.frameIndex);
}
