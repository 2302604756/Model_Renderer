#pragma once
#include "Pass.h"
#include "ProbeTypes.h"
#include <d3d12.h>

// 通用 gutter 镜像 pass：irradiance（stride=10）/ distance（stride=18）各一个实例
class ProbeBorderFixupPass : public RenderPass {
public:
    void Init(ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pso,
        RenderGraph::ResourceId atlasId,
        ID3D12Resource* atlasResource, // UAV barrier 用
        uint32_t countX, uint32_t countY, uint32_t countZ,
        uint32_t uavIndex,
        uint32_t stride,
        uint32_t atlasWidth,
        uint32_t atlasHeight);

    const char* GetName() const override { return "ProbeBorderFixup"; }
    void Declare(RenderGraph::PassBuilder& builder) override;
    void Execute(RenderGraph::FrameContext& ctx) override;

private:
    ID3D12RootSignature* m_rootSignature = nullptr;
    ID3D12PipelineState* m_pso = nullptr;
    ID3D12Resource* m_atlasResource = nullptr;

    RenderGraph::ResourceId m_atlasId = RenderGraph::kInvalidId;

    uint32_t m_countX = 0, m_countY = 0, m_countZ = 0;
    uint32_t m_uavIndex = 0;
    uint32_t m_stride = 0;
    uint32_t m_atlasWidth = 0;
    uint32_t m_atlasHeight = 0;
};