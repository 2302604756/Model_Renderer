#pragma once

#include "GiTypes.h"
#include "RenderGraph.h"
#include <memory>

//---------------------------------------------------------
// 整个GIPipeline所做就是读入GBuffer并最后输出直接光照 diffuse 间接光和 specular间接光

struct ID3D12Device;
class CPUDescriptorAllocator;
class GPUDescriptorAllocator;
class DDGI;

// resource 用于声明图依赖，srvIndex 用于绑定实际 shader view。
struct GiTextureView {
    RenderGraph::ResourceId resource = RenderGraph::kInvalidId;
    uint32_t srvIndex = UINT32_MAX;
    bool IsValid() const noexcept { return resource != RenderGraph::kInvalidId && srvIndex != UINT32_MAX; }
};

struct GBufferViews {
    GiTextureView albedo;
    GiTextureView normal;
    GiTextureView depth;
    GiTextureView volumeMask;
};

struct GiInputs {
    const GBufferViews& gbuffer;
    GiTextureView directLighting;
    GiTextureView diffuseLighting; // 镜面算法可采样已经算好的漫反射光照。
};

// 根据输入，向 RenderGraph 添加自己的 Pass，然后返回输出 GiTextureView。
class IGiProvider {
public:
    virtual ~IGiProvider() = default;
    // 交付当前视口大小的线性 RGBA16F，RGB 已应用接收面的材质响应。
    virtual GiTextureView AddPasses(RenderGraph& graph, RenderGraph::FrameContext& frame,
        const GiInputs& inputs, bool resetHistory) = 0;
};

// Pipeline不直接持有资源 而是由内部Impl持有
class GiPipeline {
public:
    GiPipeline();
    ~GiPipeline();
    GiPipeline(const GiPipeline&) = delete;
    GiPipeline& operator=(const GiPipeline&) = delete;

    void Init(ID3D12Device* device, CPUDescriptorAllocator& cpuDescriptors,
        GPUDescriptorAllocator& gpuDescriptors, RenderGraph& graph, DDGI& ddgi,
        const GBufferViews& gbuffer, uint32_t width, uint32_t height);
    void AddPasses(RenderGraph& graph, RenderGraph::FrameContext& frame,
        const RenderOptions& options, RenderGraph::ResourceId backbufferId);
    void AddProbeDebugUpdates(RenderGraph& graph, RenderGraph::FrameContext& frame, bool resetHistory);
    void FinishFrame() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
