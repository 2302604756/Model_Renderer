#pragma once
#include "Pass.h"
#include "ProbeTypes.h"
#include <d3d12.h>

class ProbeGrid;
class ProbeRayBuffer;

// 探针重定位：用本帧 fixed rays 判断探针是否陷在几何里 / 是否离正面太近，
// 算出每个探针的世界空间偏移写回 probeData buffer。帧末执行，下一帧生效。
class ProbeRelocationPass : public RenderPass {
public:
    void Init(ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pso,
        ProbeGrid* probeGrid,
        ProbeRayBuffer* rayBuffer,
        RenderGraph::ResourceId rayBufferId);

    const char* GetName() const override { return "ProbeRelocation"; }
    void Declare(RenderGraph::PassBuilder& builder) override;
    void Execute(RenderGraph::FrameContext& ctx) override;

    // 让探针重新收敛（场景/参数变了之后用）
    void Reset() { m_frameIndex = 0; }// 改场景/参数后手动重新收敛

private:
    ID3D12RootSignature* m_rootSignature = nullptr;
    ID3D12PipelineState* m_pso = nullptr;
    ProbeGrid* m_probeGrid = nullptr;
    ProbeRayBuffer* m_rayBuffer = nullptr;
    RenderGraph::ResourceId m_rayBufferId = RenderGraph::kInvalidId;
    uint32_t                m_frameIndex = 0;
};