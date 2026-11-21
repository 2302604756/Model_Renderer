#pragma once

#include <cstdint>
#include <vector>
#include "AccelerationStructure.h"
#include "BindlessDescriptorHeap.h"
#include "CPUDescriptorAllocator.h"
#include "DDGI.h"
#include "DepthBuffer.h"
#include "GBufferPass.h"
#include "GiPipeline.h"
#include "LightingPass.h"
#include "LightingTypes.h"
#include "MeshLibrary.h"
#include "RenderContext.h"
#include "RenderGraph.h"
#include "RenderTexture.h"
#include "SceneGpuData.h"
#include "Texture2D.h"
#include "TLASUpdatePass.h"
#include "TransientHeap.h"
#include "UploadHeap.h"

class Camera;
class Scene;

// 单帧 CPU 侧计时，由 App 汇总后显示在窗口标题。
struct FrameTimings {
    float waitMs = 0.0f;          // CPU 等待当前帧槽 fence 的时间。
    float beginMs = 0.0f;         // BeginFrame 总耗时，包含等待和重置。
    float recordMs = 0.0f;        // 场景 GPU 数据更新完成后，录制本帧命令的时间。
    float executeMs = 0.0f;       // 关闭命令列表并提交到队列所花的 CPU 时间。
    float presentMs = 0.0f;       // Present 调用所花的 CPU 时间。
    uint32_t waitHit = 0;         // 当前帧槽是否触发等待，取值为 0 或 1。
    bool usingGpuUpload = false;  // 当前帧槽的上传堆是否为 GPU_UPLOAD。
};

// 渲染组合根。持有 GPU 资源和 pass，借用 App 管理的 CPU 场景。
class Renderer {
public:
    // Scene 的地址和存期必须覆盖 Renderer 以及借用场景的 pass。
    explicit Renderer(Scene& scene);
    // RenderGraph 和 pass 会借用成员地址，初始化后保持对象地址固定。
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    // 创建上下文和资源、完成初始化上传，再装配帧图使用的 pass。
    void Init(HWND hwnd);
    // 相机与模式设置在录制本帧时读取。
    FrameTimings RenderFrame(const Camera& camera, float dt,
        const RenderOptions& options);
    // 返回由上下文持有的适配器名称，供窗口标题使用。
    const wchar_t* GetAdapterName() const noexcept;
    // 停止提交帧后调用，先排空队列，再清除帧图对资源和 pass 的借用记录。
    void Shutdown();

private:
    // 将场景光源打包成逐帧上传的数据和共享光照常量。
    void BuildLights();

    // 1.持有设备与上下文   先声明上下文，使其在其他成员释放之后析构。
    RenderContext m_context;
    // 上下文初始化完成即置位，使后续模块初始化失败时也能安全尝试排空队列。
    bool m_contextReady = false;
    Scene& m_scene;

    // 2.分配器与堆 CPU 描述符供 RTV/DSV 使用。所有渲染 pass 共享 GPU bindless 堆。
    CPUDescriptorAllocator m_cpuAllocator;
    GPUDescriptorAllocator m_gpuAllocator;
    TransientHeap m_physicalHeap; // 为当前示例的纹理和网格提供 placed resource 内存。
    UploadHeap m_uploadHeap;      // 初始化上传使用。逐帧常量使用 FrameResource 的上传堆。

    // 3.几何与材质 场景引用这些几何和材质 ID，由 Renderer 保证底层 GPU 资源存活。
    MeshLibrary m_meshLibrary;
    Texture2D m_texture;
    uint32_t m_cubeMeshId = 0;
    uint32_t m_sphereMeshId = 0;
    uint32_t m_planeMeshId = 0;
    uint32_t m_textureIndex = 0;
    // 4.GPU 场景和 TLAS 以相同帧槽索引选择数据和描述符。
    SceneGpuData m_sceneGpuData;
    rt::AccelerationScene m_rtScene;
    TLASUpdatePass m_tlasUpdatePass;

    std::vector<GPULight> m_gpuLights;
    LightingConstants m_lightingConstants{};
    uint32_t m_frameCounter = 0;  // 着色器逐帧采样的序号，独立于 DDGI 历史帧号。

    // 5.延迟渲染资源 延迟渲染的几何输出和供 Lighting 读取的深度 SRV。
    DepthBuffer m_depthBuffer;
    uint32_t m_depthBindlessIndex = UINT32_MAX;
    RenderTexture m_gbufferAlbedo;
    RenderTexture m_gbufferNormal;
    RenderTexture m_gbufferVolumeMask;
    // ResourceId 用于 RenderGraph 查找资源和跟踪状态，SRV 索引供着色器使用。
    RenderGraph::ResourceId m_gbufferAlbedoId = RenderGraph::kInvalidId;
    RenderGraph::ResourceId m_gbufferNormalId = RenderGraph::kInvalidId;
    RenderGraph::ResourceId m_gbufferVolumeMaskId = RenderGraph::kInvalidId;
    RenderGraph::ResourceId m_depthId = RenderGraph::kInvalidId;
    // 每个帧槽对应一个已登记的交换链缓冲。
    std::vector<RenderGraph::ResourceId> m_backbufferIds;

    // 6.图形根签名和PSO GBufferPass 和 LightingPass 借用 PSO，PSO 的所有权留在 Renderer。
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_gbufferPSO;
    ComPtr<ID3D12PipelineState> m_lightingPSO;
    GBufferPass m_gbufferPass;
    LightingPass m_lightingPass;

    // ComputeSmoke 保留的计算管线及图资源，目前未加入每帧调度。
    ComPtr<ID3D12RootSignature> m_computeRootSignature;
    ComPtr<ID3D12PipelineState> m_computePSO;
    RenderTexture m_computeTexture;
    RenderGraph::ResourceId m_computeTextureId = RenderGraph::kInvalidId;

    // 7.子模块 DDGI 私有持有图集、探针历史和更新 pass，只提供 Lighting 所需的输入参数。
    DDGI m_ddgi;
    GiPipeline m_giPipeline;
    // 图借用资源/pass。末尾声明使其先于资源所有者析构。
    RenderGraph m_graph;
};
