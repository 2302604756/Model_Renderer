#pragma once
#include <d3d12.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class RenderPass;

class RenderGraph {
public:
    using ResourceId = uint32_t;                                // 资源id
    using PassId = uint32_t;                                    // Pass 序号
    using Version = uint32_t;                                   // 本帧数据版本
    static constexpr uint32_t kInvalidId = UINT32_MAX;
    static constexpr Version kLatestVersion = UINT32_MAX;

    // 所有资源均由调用方持有。Preserved 允许读取进入本帧之前已存在的内容（版本 0）。
    
    // FrameLocal是代表初始为空必须有人写不许读版本0
    // Preserved表示初始不为空 可读版本0
    enum class ResourceContents { FrameLocal, Preserved };

    // 生命周期
    struct ResourceLifetime {
        uint32_t firstUse = kInvalidId; // 编译后的执行序号，不是登记序号
        uint32_t lastUse = kInvalidId;  // 包含最终状态转换。未使用时两者都为 kInvalidId。
    };

    // 记录PassID和barrier数组
    struct CompiledPass {
        PassId id;
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
    };

    // Renderer 每帧解析当前帧槽的 GPU 绑定，Pass 通过 FrameContext 使用。
    struct FrameBindings
    {
        uint32_t                    objectDataBindlessIndex = UINT32_MAX;
        D3D12_GPU_DESCRIPTOR_HANDLE tlasDescriptor{};
        ResourceId                 tlasResource = kInvalidId;
    };

    struct FrameContext {
        uint32_t                    frameIndex = 0;
        uint32_t                    probeFrameIndex = 0;
        bool                        resetProbeHistory = false;

        ID3D12GraphicsCommandList*  cmdList = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS   frameConstantsAddress = 0;    // b1
        D3D12_GPU_VIRTUAL_ADDRESS   lightingConstantsAddress = 0; // b3
        D3D12_GPU_VIRTUAL_ADDRESS   lightBufferAddress = 0;       // t5
        uint32_t                    lightCount = 0;
        uint32_t                    debugMode = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv{};

        FrameBindings               bindings{};
    };

    // 版本是本帧的数据依赖。同一资源的所有版本共享一个 ID3D12Resource。
    class PassBuilder {
    public:
        // 默认读取本帧最终版本。读取中间结果时，显式指定写者的版本。
        PassBuilder& Read(ResourceId id,
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            Version version = kLatestVersion);
        PassBuilder& Write(ResourceId id,
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET,
            Version version = 1);
        // 就地修改：读取 version - 1，写出 version。
        PassBuilder& ReadWrite(ResourceId id,
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            Version version = 1);
        // 资源所有读写结束后的状态，例如将交换链后缓冲切到 PRESENT。
        PassBuilder& Transition(ResourceId id, D3D12_RESOURCE_STATES state);

    private:
        friend class RenderGraph;
        // 确定是资源是哪一种依赖关系
        enum class Access { Read, Write, ReadWrite, Transition };
        // 记录了本次操作的资源id state version access
        struct Usage {
            ResourceId id;
            D3D12_RESOURCE_STATES state;
            Version version;
            Access access;
            bool operator==(const Usage&) const = default;
        };

        PassBuilder& AddUsage(Usage usage);
        std::vector<Usage> m_usages;
    };

    ResourceId RegisterResource(const char* name, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES initialState, ResourceContents contents = ResourceContents::FrameLocal);

    // 本帧版本和生命周期重新计算。GPU 资源状态跨帧保留。
    void BeginFrame();

    void AddPass(RenderPass& pass);
    // 在不改变原本模块接口的情况下也能额外声明 读写关系
    void AddPass(RenderPass& pass, std::function<void(PassBuilder&)> extraSetup);
    void AddPass(std::string name,
        std::function<void(PassBuilder&)> setup,
        std::function<void(FrameContext&)> execute);

    // 在录制任何 Pass 前完成校验、排序与 barrier 规划。错误通过 std::logic_error 报告。
    void Compile();
    void Execute(FrameContext& ctx);

    ID3D12Resource* GetResource(ResourceId id) const { return m_resources.at(id).resource; }
    D3D12_RESOURCE_STATES GetState(ResourceId id) const { return m_resources.at(id).tracking.state; }
    // 查询本帧 Compile 或 Execute 生成的计划。
    const std::string& GetPassName(PassId id) const { return m_passes.at(id).name; }
    const std::vector<CompiledPass>& GetCompiledPasses() const { return m_compiledPasses; }
    ResourceLifetime GetLifetime(ResourceId id) const { return m_lifetimes.at(id); }

private:
    struct ResourceState {
        D3D12_RESOURCE_STATES state;
        bool unorderedAccess = false;
        bool unorderedWrite = false;
    };

    // 资源名称 指针 资源初始情况 资源状态
    struct ResourceEntry {
        std::string name;
        ID3D12Resource* resource;
        ResourceContents contents;
        ResourceState tracking;
    };

    // Pass名 整个Pass资源usage 执行function
    struct PassEntry {
        std::string name;
        std::vector<PassBuilder::Usage> usages;
        std::function<void(FrameContext&)> execute;
    };

    void BuildBarrierPlan(const std::vector<PassId>& order);

    std::vector<ResourceEntry> m_resources;             // 在Renderer中被注册
    std::vector<PassEntry> m_passes;                    // 在AddPass中被写
    std::vector<CompiledPass> m_compiledPasses;         // 在buildbarrier中被初始化
    std::vector<ResourceLifetime> m_lifetimes;
    std::vector<ResourceState> m_finalStates;
    bool m_compiled = false;
};
