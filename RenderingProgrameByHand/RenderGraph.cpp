#include "RenderGraph.h"
#include "Pass.h"
#include <cassert>


RenderGraph::PassBuilder& RenderGraph::PassBuilder::Read(ResourceId id, D3D12_RESOURCE_STATES state)
{
    m_usages.push_back({ id, state });
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::Write(ResourceId id, D3D12_RESOURCE_STATES state)
{
    m_usages.push_back({ id, state });
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadWrite(ResourceId id, D3D12_RESOURCE_STATES state)
{
    m_usages.push_back({ id, state });
    return *this;
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::Transition(ResourceId id, D3D12_RESOURCE_STATES state)
{
    m_usages.push_back({ id, state });
    return *this;
}

void RenderGraph::Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    cmdList->ResourceBarrier(1, &barrier);
}

RenderGraph::ResourceId RenderGraph::RegisterResource(const char* name, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES initialState)
{
    m_resources.push_back({ name, resource, initialState });
    return static_cast<ResourceId>(m_resources.size() - 1);
}

//清空Pass 后续我们可以做动态的Pass
void RenderGraph::BeginFrame()
{
    m_passes.clear();
}

void RenderGraph::AddPass(RenderPass& pass)
{
    PassBuilder builder;
    pass.Declare(builder);

    PassEntry entry;
    entry.name = pass.GetName();
    entry.usages = builder.GetUsages();

    RenderPass* p = &pass;
    entry.execute = [p](FrameContext& ctx) { p->Execute(ctx); };

    m_passes.push_back(std::move(entry));
}

void RenderGraph::AddPass(std::string name,
    std::function<void(PassBuilder&)> setup,
    std::function<void(FrameContext&)> execute)
{
    PassBuilder builder;
    setup(builder);

    PassEntry entry;
    entry.name = std::move(name);
    entry.usages = builder.GetUsages();
    entry.execute = std::move(execute);

    m_passes.push_back(std::move(entry));
}

void RenderGraph::Execute(FrameContext& ctx)
{
    for (auto& pass : m_passes)
    {
        // 同一 pass 里同一资源声明多次时，以最后一次为准
        for (size_t i = 0; i < pass.usages.size(); ++i)
        {
            const auto& usage = pass.usages[i];

            // 防御：pass 声明了无效资源 id
            if (usage.id == kInvalidId || usage.id >= m_resources.size())
            {
                assert(false && "Pass declared an invalid resource id");
                continue;
            }

            bool overridden = false;
            for (size_t j = i + 1; j < pass.usages.size(); ++j)
            {
                if (pass.usages[j].id == usage.id) { overridden = true; break; }
            }
            if (overridden) continue;

            //能进行到下面一步说明是最后一次声明
            ResourceEntry& entry = m_resources[usage.id];
            if (entry.state != usage.state)
            {
                //把每一个要用到的资源barrier设置完毕
                Transition(ctx.cmdList, entry.resource, entry.state, usage.state);
                entry.state = usage.state;
            }
        }

        pass.execute(ctx);
    }
}