#include "GiPipeline.h"
#include "DDGI.h"
#include "LightingTypes.h"
#include "Pass.h"
#include "PipelineBuilder.h"
#include "RenderTexture.h"
#include "RootSignature.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <initializer_list>

namespace {
    constexpr DXGI_FORMAT kGiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr uint32_t kHasDiffuse = 1u;
    constexpr uint32_t kHasSpecular = 2u;
    constexpr uint32_t kEnableDither = 4u;

    // 对应 GiPassBindings.hlsli 的 b5。旧 b2 继续供 DDGI 采样和直接光照使用。
    struct GiPassParams {
        uint32_t directLightingIndex = 0;
        uint32_t diffuseGiIndex = 0;
        uint32_t specularGiIndex = 0;
        uint32_t displayMode = 0;
        uint32_t flags = 0;
        uint32_t padding[3]{};
    };
    static_assert(sizeof(GiPassParams) == 8 * sizeof(uint32_t));

    struct LightingTarget {
        RenderTexture texture;
        GiTextureView view;

        void Init(ID3D12Device* device, CPUDescriptorAllocator& cpu, GPUDescriptorAllocator& gpu,
            RenderGraph& graph, uint32_t width, uint32_t height, const char* name)
        {
            const float clear[4]{};
            texture.Init(device, cpu, gpu, width, height, kGiFormat, clear);
            view = { graph.RegisterResource(name, texture.GetResource(), texture.GetState()),
                texture.GetBindlessIndex() };
        }
    };

    
    LightingParams GBufferParams(const GBufferViews& views)
    {
        LightingParams lighting{};
        lighting.albedoIndex = views.albedo.srvIndex;
        lighting.normalIndex = views.normal.srvIndex;
        lighting.depthIndex = views.depth.srvIndex;
        return lighting;
    }

    // 真正写入RenderGraph执行的Pass
    class FullscreenPass final : public RenderPass {
    public:
        void Init(ID3D12Device* device, ID3D12RootSignature* root, const std::vector<uint8_t>& vs,
            const wchar_t* file, const wchar_t* entry, const char* name, DXGI_FORMAT format = kGiFormat)
        {
            m_root = root;
            m_name = name;
            const auto ps = d3d12lite::CompileShaderSM66(file, entry, L"ps_6_6");
            m_pso = PipelineBuilder().SetRootSignature(root).SetShaders(vs, ps)
                .SetRTVFormat(format).SetDepthDisabled().SetCullNone().Build(device);
        }

        void Schedule(RenderGraph& graph, const LightingParams& lighting, const GiPassParams& gi,
            std::initializer_list<GiTextureView> inputs, RenderGraph::ResourceId output,
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = {})
        {
            assert(inputs.size() <= m_inputs.size());
            m_inputs.fill({});
            std::copy(inputs.begin(), inputs.end(), m_inputs.begin());
            m_lighting = lighting;
            m_gi = gi;
            m_output = output;
            m_rtv = rtv;
            graph.AddPass(*this);
        }

        const char* GetName() const override { return m_name; }

        void Declare(RenderGraph::PassBuilder& builder) override
        {
            for (const GiTextureView& input : m_inputs)
                if (input.IsValid())
                    builder.Read(input.resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            builder.Write(m_output, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }

        void Execute(RenderGraph::FrameContext& frame) override
        {
            auto* cmd = frame.cmdList;
            const auto target = m_rtv.ptr ? m_rtv : frame.backbufferRtv;
            cmd->OMSetRenderTargets(1, &target, FALSE, nullptr);
            cmd->SetGraphicsRootSignature(m_root);
            cmd->SetPipelineState(m_pso.Get());
            cmd->SetGraphicsRootConstantBufferView(0, frame.frameConstantsAddress);
            LightingParams lighting = m_lighting;
            lighting.objectIndex = frame.bindings.objectDataBindlessIndex;
            lighting.lightCount = frame.lightCount;
            cmd->SetGraphicsRoot32BitConstants(1, 16, &lighting, 0);
            cmd->SetGraphicsRootDescriptorTable(2, frame.bindings.tlasDescriptor);
            cmd->SetGraphicsRootConstantBufferView(3, frame.lightingConstantsAddress);
            cmd->SetGraphicsRootShaderResourceView(4, frame.lightBufferAddress);
            cmd->SetGraphicsRoot32BitConstants(5, 8, &m_gi, 0);
            // 每个 PS 为整个视口写入有效结果，背景和未命中由 PS 写零。
            cmd->DrawInstanced(3, 1, 0, 0);
        }

    private:
        const char* m_name = nullptr;
        ID3D12RootSignature* m_root = nullptr;
        ComPtr<ID3D12PipelineState> m_pso;
        std::array<GiTextureView, 6> m_inputs{};
        LightingParams m_lighting{};
        GiPassParams m_gi{};
        RenderGraph::ResourceId m_output = RenderGraph::kInvalidId;
        D3D12_CPU_DESCRIPTOR_HANDLE m_rtv{};
    };

    // 借用ddgi系统 写入diffuseTarget 
    class ProbeGiProvider final : public IGiProvider {
    public:
        void Init(DDGI& ddgi, LightingTarget& target, ID3D12Device* device,
            ID3D12RootSignature* root, const std::vector<uint8_t>& vs)
        {
            m_ddgi = &ddgi;
            m_target = &target;
            m_resolve.Init(device, root, vs, L"ShadersLighting.hlsl", L"DDGIResolvePS", "DDGIResolve");
        }

        void AddProbeUpdates(RenderGraph& graph, RenderGraph::FrameContext& frame, bool resetHistory)
        {
            // 模块重新启用时，以当前场景重新收敛探针。
            if (resetHistory || !m_updatedLastFrame)
                m_ddgi->RequestHistoryReset();
            m_ddgi->PrepareFrame(frame);
            m_ddgi->AddUpdatePasses(graph);
            m_updatedThisFrame = true;
        }

        GiTextureView AddPasses(RenderGraph& graph, RenderGraph::FrameContext& frame,
            const GiInputs& inputs, bool resetHistory) override
        {
            AddProbeUpdates(graph, frame, resetHistory);
            const auto probes = m_ddgi->GetLightingInputs();
            LightingParams lighting = GBufferParams(inputs.gbuffer);
            lighting.gridOriginSpacing = {
                probes.gridOrigin.x, probes.gridOrigin.y, probes.gridOrigin.z, probes.gridSpacing };
            lighting.gridCountX = probes.gridCounts.x;
            lighting.gridCountY = probes.gridCounts.y;
            lighting.gridCountZ = probes.gridCounts.z;
            lighting.probeSrvIndex = probes.filteredSrv;
            lighting.distanceAtlasIndex = probes.distanceSrv;
            m_resolve.Schedule(graph, lighting, {},
                { inputs.gbuffer.albedo, inputs.gbuffer.normal, inputs.gbuffer.depth,
                  inputs.gbuffer.volumeMask, { probes.filteredAtlas, probes.filteredSrv },
                  { probes.distanceAtlas, probes.distanceSrv } },
                m_target->view.resource, m_target->texture.GetRTV());
            return m_target->view;
        }

        void FinishFrame() noexcept
        {
            if (m_updatedThisFrame)
                m_ddgi->FinishFrame();
            m_updatedLastFrame = m_updatedThisFrame;
            m_updatedThisFrame = false;
        }

    private:
        DDGI* m_ddgi = nullptr;
        LightingTarget* m_target = nullptr;
        FullscreenPass m_resolve;
        bool m_updatedThisFrame = false;
        bool m_updatedLastFrame = false;
    };

    enum class LightingSource { None, Direct, DirectAndDiffuse };

    class FullscreenGiProvider final : public IGiProvider {
    public:
        void Init(LightingTarget& target, ID3D12Device* device, ID3D12RootSignature* root,
            const std::vector<uint8_t>& vs, const wchar_t* file, const wchar_t* entry,
            const char* name, LightingSource source)
        {
            m_target = &target;
            m_source = source;
            m_pass.Init(device, root, vs, file, entry, name);
        }

        GiTextureView AddPasses(RenderGraph& graph, RenderGraph::FrameContext&,
            const GiInputs& inputs, bool) override
        {
            const GiTextureView direct = m_source != LightingSource::None ? inputs.directLighting : GiTextureView{};
            const GiTextureView diffuse = m_source == LightingSource::DirectAndDiffuse
                ? inputs.diffuseLighting : GiTextureView{};
            GiPassParams params{};
            if (direct.IsValid())
                params.directLightingIndex = direct.srvIndex;
            if (diffuse.IsValid()) {
                params.diffuseGiIndex = diffuse.srvIndex;
                params.flags |= kHasDiffuse;
            }
            m_pass.Schedule(graph, GBufferParams(inputs.gbuffer), params,
                { inputs.gbuffer.albedo, inputs.gbuffer.normal, inputs.gbuffer.depth, direct, diffuse },
                m_target->view.resource, m_target->texture.GetRTV());
            return m_target->view;
        }

    private:
        LightingTarget* m_target = nullptr;
        LightingSource m_source = LightingSource::None;
        FullscreenPass m_pass;
    };
}

struct GiPipeline::Impl {
    IGiProvider* SelectDiffuse(DiffuseGiMode mode) noexcept
    {
        switch (mode) {
        case DiffuseGiMode::DDGI: return &probes;
        case DiffuseGiMode::SSGI: return &screenDiffuse;
        case DiffuseGiMode::RayTraced: return &rayTraced;
        case DiffuseGiMode::None: return nullptr;
        }
        return nullptr;
    }

    IGiProvider* SelectSpecular(SpecularGiMode mode) noexcept
    {
        return mode == SpecularGiMode::SSR ? &screenSpecular : nullptr;
    }
        
    GBufferViews gbuffer{};             // GBuffer
    ComPtr<ID3D12RootSignature> root;
    LightingTarget directTarget;        // 持有RenderTexture和rendergraph freelist id 和 bindless heap id
    LightingTarget diffuseTarget;       // 仅选中的漫反射实现写入此目标。
    LightingTarget specularTarget;
    FullscreenPass direct;      
    FullscreenPass composite;
    ProbeGiProvider probes;
    FullscreenGiProvider rayTraced;
    FullscreenGiProvider screenDiffuse;
    FullscreenGiProvider screenSpecular;
};

GiPipeline::GiPipeline() = default;
GiPipeline::~GiPipeline() = default;

void GiPipeline::Init(ID3D12Device* device, CPUDescriptorAllocator& cpuDescriptors,
    GPUDescriptorAllocator& gpuDescriptors, RenderGraph& graph, DDGI& ddgi,
    const GBufferViews& gbuffer, uint32_t width, uint32_t height)
{
    m_impl = std::make_unique<Impl>();
    auto& p = *m_impl;
    p.gbuffer = gbuffer;
    p.directTarget.Init(device, cpuDescriptors, gpuDescriptors, graph, width, height, "DirectLightingHDR");
    p.diffuseTarget.Init(device, cpuDescriptors, gpuDescriptors, graph, width, height, "DiffuseGIHDR");
    p.specularTarget.Init(device, cpuDescriptors, gpuDescriptors, graph, width, height, "SpecularGIHDR");

    // 独立的全屏根签名。复用 b1/b2/b3/t3/t5 的布局，为通用纹理输入添加 b5。
    p.root = RootSignatureBuilder().AddRootCBV(1).Add32BitConstants(16, 2)
        .AddRootDescriptorTable(3).AddRootCBV(3).AddRootSRV(5)
        .Add32BitConstants(8, 5).AddDefaultStaticSamplers().Build(device, false);
    const auto vs = d3d12lite::CompileShaderSM66(L"ShadersLighting.hlsl", L"LightingVS", L"vs_6_6");
    p.direct.Init(device, p.root.Get(), vs, L"ShadersLighting.hlsl", L"DirectLightingPS", "DirectLighting");
    p.composite.Init(device, p.root.Get(), vs, L"ShadersLighting.hlsl", L"CompositeLightingPS",
        "CompositeLighting", DXGI_FORMAT_R8G8B8A8_UNORM);
    p.probes.Init(ddgi, p.diffuseTarget, device, p.root.Get(), vs);
    p.rayTraced.Init(p.diffuseTarget, device, p.root.Get(), vs, L"ShadersLighting.hlsl",
        L"RayTracedDiffusePS", "RayTracedDiffuseGI", LightingSource::None);
    p.screenDiffuse.Init(p.diffuseTarget, device, p.root.Get(), vs, L"ShadersScreenSpaceGI.hlsl",
        L"ScreenSpaceDiffusePS", "ScreenSpaceDiffuseGI", LightingSource::Direct);
    p.screenSpecular.Init(p.specularTarget, device, p.root.Get(), vs, L"ShadersScreenSpaceGI.hlsl",
        L"ScreenSpaceSpecularPS", "ScreenSpaceSpecularGI", LightingSource::DirectAndDiffuse);
}

void GiPipeline::AddPasses(RenderGraph& graph, RenderGraph::FrameContext& frame,
    const RenderOptions& options, RenderGraph::ResourceId backbufferId)
{
    auto& p = *m_impl;
    p.direct.Schedule(graph, GBufferParams(p.gbuffer), {},
        { p.gbuffer.albedo, p.gbuffer.normal, p.gbuffer.depth },
        p.directTarget.view.resource, p.directTarget.texture.GetRTV());

    GiInputs inputs{ p.gbuffer, p.directTarget.view, {} };
    if (auto* diffuse = p.SelectDiffuse(options.diffuse))
        inputs.diffuseLighting = diffuse->AddPasses(graph, frame, inputs, options.resetProbeHistory);

    GiTextureView specularOutput{};
    if (auto* specular = p.SelectSpecular(options.specular))
        specularOutput = specular->AddPasses(graph, frame, inputs, false);

    // Off 对应零贡献，合成着色器根据标志跳过读取该输入。
    GiPassParams compose{};
    compose.directLightingIndex = p.directTarget.view.srvIndex;
    compose.displayMode = static_cast<uint32_t>(options.display);
    if (inputs.diffuseLighting.IsValid()) {
        compose.diffuseGiIndex = inputs.diffuseLighting.srvIndex;
        compose.flags |= kHasDiffuse;
    }
    if (specularOutput.IsValid()) {
        compose.specularGiIndex = specularOutput.srvIndex;
        compose.flags |= kHasSpecular;
    }
    // 保留原 DDGI 最终输出的抖动以及 RT 对照路径的输出行为。
    if (options.display == GiDisplayMode::Composite && options.diffuse != DiffuseGiMode::RayTraced)
        compose.flags |= kEnableDither;
    p.composite.Schedule(graph, {}, compose,
        { p.directTarget.view, inputs.diffuseLighting, specularOutput }, backbufferId);
}

void GiPipeline::AddProbeDebugUpdates(RenderGraph& graph, RenderGraph::FrameContext& frame,
    bool resetHistory)
{
    m_impl->probes.AddProbeUpdates(graph, frame, resetHistory);
}

void GiPipeline::FinishFrame() noexcept
{
    m_impl->probes.FinishFrame();
}
