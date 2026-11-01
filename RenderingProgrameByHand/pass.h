#pragma once
#include "RenderGraph.h"

class RenderPass {
public:
    virtual ~RenderPass() = default;

    virtual const char* GetName() const = 0;
    virtual void Declare(RenderGraph::PassBuilder& builder) = 0;        //声明
    virtual void Execute(RenderGraph::FrameContext& ctx) = 0;           //执行
};