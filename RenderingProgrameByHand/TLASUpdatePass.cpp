#include "TLASUpdatePass.h"
#include "AccelerationStructure.h"
#include "Scene.h"

void TLASUpdatePass::Init(rt::AccelerationScene* rtScene, const Scene* scene)
{
    m_rtScene = rtScene;
    m_scene = scene;
}

void TLASUpdatePass::Execute(RenderGraph::FrameContext& ctx)
{
    m_rtScene->UpdateTLAS(ctx.cmdList, m_scene->GetObjects(), ctx.frameIndex);
}