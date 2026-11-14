#pragma once
#include "Pass.h"

namespace rt { class AccelerationScene; }
class Scene;

// 每帧重建 TLAS，必须在所有光追 pass 之前
class TLASUpdatePass : public RenderPass {
public:
    void Init(rt::AccelerationScene* rtScene, const Scene* scene);

    const char* GetName() const override { return "TLASUpdate"; }
    void Declare(RenderGraph::PassBuilder& builder) override {}
    void Execute(RenderGraph::FrameContext& ctx) override;

private:
    rt::AccelerationScene* m_rtScene = nullptr;
    const Scene* m_scene = nullptr;
};
