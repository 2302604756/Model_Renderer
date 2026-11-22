#pragma once
#include "Pass.h"

namespace rt { class AccelerationScene; }
class Scene;

// 每帧重建当前帧槽的 TLAS，光追 pass 通过资源声明依赖这个结果。
class TLASUpdatePass : public RenderPass {
public:
    void Init(rt::AccelerationScene* rtScene, const Scene* scene);
    void Schedule(RenderGraph& graph, RenderGraph::ResourceId tlas);

    const char* GetName() const override { return "TLASUpdate"; }
    void Declare(RenderGraph::PassBuilder& builder) override;
    void Execute(RenderGraph::FrameContext& ctx) override;

private:
    rt::AccelerationScene* m_rtScene = nullptr;
    const Scene* m_scene = nullptr;
    RenderGraph::ResourceId m_tlas = RenderGraph::kInvalidId;
};
