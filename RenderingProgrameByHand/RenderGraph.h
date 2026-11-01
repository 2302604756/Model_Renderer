#pragma once
#include <d3d12.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class RenderPass;

class RenderGraph {
public:
	using ResourceId = uint32_t;
	static constexpr ResourceId kInvalidId = UINT32_MAX;

    // 每次 Execute 时传给每个 Pass 的每帧数据
    struct FrameContext {
        uint32_t                    frameIndex = 0;
        ID3D12GraphicsCommandList*  cmdList = nullptr;

        D3D12_GPU_VIRTUAL_ADDRESS   frameConstantsAddress = 0;    // b1
        D3D12_GPU_VIRTUAL_ADDRESS   lightingConstantsAddress = 0; // b3
        D3D12_GPU_VIRTUAL_ADDRESS   lightBufferAddress = 0;       // t5
        uint32_t                    lightCount = 0;
        uint32_t                    debugMode = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv{};
    };

    // Pass 内声明资源读写
    //PassBuilder就是用于在声明阶段写入ResourceID以及操作类型的
    //PassBuilder不常驻 在需要时随地创建然后把m_usages写到Render Graph的m_passes中
    class PassBuilder {
    public:
        PassBuilder& Read(ResourceId id,
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        PassBuilder& Write(ResourceId id,
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET);
        PassBuilder& ReadWrite(ResourceId id,
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        PassBuilder& Transition(ResourceId id, D3D12_RESOURCE_STATES state); // 只切状态，不读写

        struct Usage { ResourceId id; D3D12_RESOURCE_STATES state; };                       //usage用来存储ResourceID和Resource State
        const std::vector<Usage>& GetUsages() const { return m_usages; }

    private:
        std::vector<Usage> m_usages;
    };

    ResourceId RegisterResource(const char* name, ID3D12Resource* resource,                 //只是在m_resources中占据对应索引
        D3D12_RESOURCE_STATES initialState);

    // 每帧开始时清空 pass 列表，资源状态（m_resources）跨帧保留
    void BeginFrame();

    void AddPass(RenderPass& pass);
    void AddPass(std::string name,
        std::function<void(PassBuilder&)> setup,
        std::function<void(FrameContext&)> execute);

    void Execute(FrameContext& ctx);

    ID3D12Resource* GetResource(ResourceId id) const { return m_resources[id].resource; }
    D3D12_RESOURCE_STATES GetState(ResourceId id) const { return m_resources[id].state; }

private:
    //资源注册表
    //配合RegisterResource在init时调用一次
    struct ResourceEntry {
        std::string            name;
        ID3D12Resource*        resource = nullptr;
        D3D12_RESOURCE_STATES  state = D3D12_RESOURCE_STATE_COMMON;
    };
    //Pass 列表 + 自动 barrier
    struct PassEntry {
        std::string                               name;
        //struct Usage { ResourceId id; D3D12_RESOURCE_STATES state; };             //usage用来存储ResourceID和Resource State
        std::vector<PassBuilder::Usage>           usages;
        std::function<void(FrameContext&)>        execute;
    };

    static void Transition(ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before,
        D3D12_RESOURCE_STATES after);
                                                                                    
    std::vector<ResourceEntry> m_resources;                                         //在初始化Init阶段 RegisterResource把东西填入到m_resources
                                                                                    //Write/Read往usage中写入的就是m_resources的id，在最后执行时去根据ID来对相应的资源进行Barrier
    std::vector<PassEntry>     m_passes;                                            //在AddPass构建时把Pass信息传入
};