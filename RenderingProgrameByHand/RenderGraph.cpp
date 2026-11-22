#include "RenderGraph.h"
#include "Pass.h"
#include <algorithm>
#include <queue>
#include <stdexcept>
#include <utility>

namespace {
    bool IsAccelerationStructure(D3D12_RESOURCE_STATES state)
    {
        return state == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    }

    bool NeedsUnorderedSync(D3D12_RESOURCE_STATES state)
    {
        return state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS || IsAccelerationStructure(state);
    }
}

RenderGraph::PassBuilder& RenderGraph::PassBuilder::AddUsage(Usage usage)
{
    if (std::find(m_usages.begin(), m_usages.end(), usage) == m_usages.end())
        m_usages.push_back(usage);
    return *this;
}

// 都是往Builder的m_usages里写
RenderGraph::PassBuilder& RenderGraph::PassBuilder::Read(ResourceId id,
    D3D12_RESOURCE_STATES state, Version version)
{
    return AddUsage({ id, state, version, Access::Read });
}

// 都是往Builder的m_usages里写
RenderGraph::PassBuilder& RenderGraph::PassBuilder::Write(ResourceId id,
    D3D12_RESOURCE_STATES state, Version version)
{
    return AddUsage({ id, state, version, Access::Write });
}

// 都是往Builder的m_usages里写
RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadWrite(ResourceId id,
    D3D12_RESOURCE_STATES state, Version version)
{
    return AddUsage({ id, state, version, Access::ReadWrite });
}

// 都是往Builder的m_usages里写
RenderGraph::PassBuilder& RenderGraph::PassBuilder::Transition(ResourceId id,
    D3D12_RESOURCE_STATES state)
{
    return AddUsage({ id, state, 0, Access::Transition });
}

// 在Renderer中注册资源
RenderGraph::ResourceId RenderGraph::RegisterResource(const char* name, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES initialState, ResourceContents contents)
{
    if (!resource)
        throw std::logic_error(std::string("RenderGraph: null resource: ") + name);
    for (const auto& entry : m_resources)
        if (entry.resource == resource)
            throw std::logic_error(std::string("RenderGraph: duplicate resource: ") + name +
                " is already registered as " + entry.name);

    m_resources.push_back({ name, resource, contents, { initialState } });
    m_compiled = false;
    return static_cast<ResourceId>(m_resources.size() - 1);
}

void RenderGraph::BeginFrame()
{
    m_passes.clear();
    m_compiledPasses.clear();
    m_lifetimes.clear();
    m_compiled = false;
}

// 先调用Pass的Declare 再往graph的m_pass写Declare到Builder的usage
void RenderGraph::AddPass(RenderPass& pass)
{
    AddPass(pass, {});
}

void RenderGraph::AddPass(RenderPass& pass, std::function<void(PassBuilder&)> extraSetup)
{
    PassBuilder builder;
    pass.Declare(builder);
    if (extraSetup)
        extraSetup(builder);
    m_passes.push_back({ pass.GetName(), std::move(builder.m_usages),
        [p = &pass](FrameContext& ctx) { p->Execute(ctx); } });
    m_compiled = false;
}

void RenderGraph::AddPass(std::string name,
    std::function<void(PassBuilder&)> setup,
    std::function<void(FrameContext&)> execute)
{
    PassBuilder builder;
    setup(builder);
    m_passes.push_back({ std::move(name), std::move(builder.m_usages), std::move(execute) });
    m_compiled = false;
}

void RenderGraph::Compile()
{
    using Access = PassBuilder::Access;

    // 记录资源被使用的PassID 和 具体的操作行为
    struct ResourceUse {
        PassId pass;
        const PassBuilder::Usage* usage;
    };

    m_compiled = false;
    m_compiledPasses.clear();
    m_lifetimes.clear();

    // 按照resourceid来记录被使用的PassID和usage
    // 这一步是记录所有usage
    std::vector<std::vector<ResourceUse>> resourceUses(m_resources.size());
    for (PassId pass = 0; pass < m_passes.size(); ++pass)
    {
        for (const auto& usage : m_passes[pass].usages)
        {
            // 使用的resource id未被注册
            if (usage.id >= m_resources.size())
                throw std::logic_error("RenderGraph: invalid resource ID in pass " + m_passes[pass].name);
            const auto& resource = m_resources[usage.id];
            if (IsAccelerationStructure(resource.tracking.state) != IsAccelerationStructure(usage.state))
                throw std::logic_error("RenderGraph: acceleration structure cannot change state: " +
                    resource.name + " in pass " + m_passes[pass].name);
            auto& uses = resourceUses[usage.id];
            // m_passes中记录PassID与usages中PassID不符
            // 你在同一个 Pass 内部，针对同一个资源（Resource），连续声明了两次完全相同的使用方式。 
            if (!uses.empty() && uses.back().pass == pass)
                throw std::logic_error("RenderGraph: conflicting declarations of " +
                    resource.name + " in pass " + m_passes[pass].name);
            uses.push_back({ pass, &usage });
        }
    }

    // 按PassID记录每个Pass的边数组 
    std::vector<std::vector<PassId>> outgoing(m_passes.size());
    // 入度数组
    std::vector<uint32_t> indegree(m_passes.size());
    // 记录 from -> to 的边 并入度加一
    const auto addEdge = [&](PassId from, PassId to)
    {
        auto& edges = outgoing[from];
        if (std::find(edges.begin(), edges.end(), to) == edges.end())
        {
            edges.push_back(to);
            ++indegree[to];
        }
    };

    // 以resource version为索引 每次循环都从头写入PassID
    std::vector<PassId> writers;
    // 逐Resource遍历
    for (ResourceId id = 0; id < resourceUses.size(); ++id)
    {
        // 对应id resource的所有usage
        const auto& uses = resourceUses[id];
        if (uses.empty())
            continue;

        const auto& resource = m_resources[id];
        // 清空容器为一个kInvalidId
        writers.assign(1, kInvalidId); // 版本 0 代表进入本帧前的内容，没有图内写者。
        // 记录最终状态转换的PassID
        PassId finalTransition = kInvalidId;
        // 逐usage遍历 记录这个Resource Version的使用PassID
        for (const auto& use : uses)
        {
            const auto& usage = *use.usage;
            // 确保一个资源在整张渲染图里，最多只能有一个 最终的状态转换
            if (usage.access == Access::Transition)
            {
                if (finalTransition != kInvalidId)
                    throw std::logic_error("RenderGraph: multiple final transitions for " + resource.name);
                finalTransition = use.pass;
            }
            // 如果这个 Pass 对资源只是纯读取（Read），直接跳过，因为读操作不会改变数据。
            if (usage.access != Access::Write && usage.access != Access::ReadWrite)
                continue;
            // 版本号必须从1开始
            if (usage.version == 0 || usage.version > uses.size())
                throw std::logic_error("RenderGraph: invalid write version for " + resource.name +
                    " in pass " + m_passes[use.pass].name);

            if (usage.version >= writers.size())
                writers.resize(usage.version + 1, kInvalidId);
            auto& writer = writers[usage.version];
            // 不准同一个版本资源被多次写入
            if (writer != kInvalidId)
                throw std::logic_error("RenderGraph: multiple writers for " + resource.name +
                    " v" + std::to_string(usage.version) + ": " + m_passes[writer].name +
                    ", " + m_passes[use.pass].name);
            // 最终写入该版本在哪个Pass被写入
            writer = use.pass;
        }

        // 记录一共有多少个版本号
        const auto latest = static_cast<Version>(writers.size() - 1);
        // 遍历该资源的所有版本号
        for (Version version = 1; version <= latest; ++version)
        {
            // 说明资源该版本未被写入
            if (writers[version] == kInvalidId)
                throw std::logic_error("RenderGraph: missing writer for " + resource.name +
                    " v" + std::to_string(version));
            // 不同版本共享底层资源，因此写入仍然必须按版本顺序完成。
            if (version > 1)
                // 建立从version-1 Pass到version Pass的边
                addEdge(writers[version - 1], writers[version]);
        }

        // 遍历该resource的所有usages
        for (const auto& use : uses)
        {
            const auto& usage = *use.usage;
            // 建立所有Pass到最终Transition Pass的边
            if (finalTransition != kInvalidId && usage.access != Access::Transition)
                addEdge(use.pass, finalTransition);
            // 如果不进行读取操作就跳过
            if (usage.access != Access::Read && usage.access != Access::ReadWrite)
                continue;
            // 写操作会改变版本号，而这一个pass的读操作是去读上一个版本
            const Version requested = usage.access == Access::ReadWrite ? usage.version - 1 : usage.version;
            // 有的 Pass 直接声明“我要读最新版”。这里会把它解析成当前实际最新的版本号 latest
            const Version version = requested == kLatestVersion ? latest : requested;
            // 检测版本号不能大于最新的版本号
            if (version > latest || (version == 0 && resource.contents == ResourceContents::FrameLocal))
                throw std::logic_error("RenderGraph: pass " + m_passes[use.pass].name +
                    " reads " + resource.name + " v" + std::to_string(version) + " without a producer");

            if (version > 0)
                // 建立写后读
                addEdge(writers[version], use.pass); // RAW
            if (version < latest && writers[version + 1] != use.pass)
                addEdge(use.pass, writers[version + 1]); // WAR：读旧版本完成后才允许覆盖。
        }
    }

    // 先执行入度为 0 的 Pass。同样可执行时，按登记序号打破平局以保持确定性。
    // 优先队列二叉堆
    std::priority_queue<PassId, std::vector<PassId>, std::greater<PassId>> ready;
    for (PassId pass = 0; pass < m_passes.size(); ++pass)
        if (indegree[pass] == 0)
            ready.push(pass);

    std::vector<PassId> order;
    order.reserve(m_passes.size());
    while (!ready.empty())
    {
        // 将就绪Pass依次弹出
        const PassId pass = ready.top();
        ready.pop();
        order.push_back(pass);
        for (const PassId next : outgoing[pass])
            // 遍历就绪Pass的所有指向Pass  并使他们的入度都减一 如果他们入度减为零了就设为就绪
            if (--indegree[next] == 0)
                ready.push(next);
    }
    // 如果order的size小于总Pass数量，则存在循环依赖
    if (order.size() != m_passes.size())
    {
        std::string message = "RenderGraph: dependency cycle; blocked passes:";
        // 入度每减到0的都有循环依赖
        for (PassId pass = 0; pass < m_passes.size(); ++pass)
            if (indegree[pass] != 0)
                message += " " + m_passes[pass].name;
        throw std::logic_error(message);
    }

    // 把排序好的order 
    BuildBarrierPlan(order);
    m_compiled = true;
}

// 在compile完成后调用
void RenderGraph::BuildBarrierPlan(const std::vector<PassId>& order)
{
    using Access = PassBuilder::Access;

    m_finalStates.clear();
    m_finalStates.reserve(m_resources.size());
    for (const auto& resource : m_resources)
        m_finalStates.push_back(resource.tracking);

    m_lifetimes.resize(m_resources.size());
    m_compiledPasses.reserve(order.size());
    for (const PassId pass : order)
    {
        const auto index = static_cast<uint32_t>(m_compiledPasses.size());
        auto& planned = m_compiledPasses.emplace_back(CompiledPass{ pass, {} });
        for (const auto& usage : m_passes[pass].usages)
        {
            auto& lifetime = m_lifetimes[usage.id];
            if (lifetime.firstUse == kInvalidId)
                lifetime.firstUse = index;
            lifetime.lastUse = index;

            const auto& resource = m_resources[usage.id];
            auto& previous = m_finalStates[usage.id];
            const bool isAccess = usage.access != Access::Transition;
            const bool writes = usage.access == Access::Write || usage.access == Access::ReadWrite;
            if (previous.state != usage.state)
            {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition = { resource.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    previous.state, usage.state };
                planned.barriers.push_back(barrier);
                previous = { usage.state };
            }
            else if (isAccess && previous.unorderedAccess && (previous.unorderedWrite || writes))
            {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                barrier.UAV.pResource = resource.resource;
                planned.barriers.push_back(barrier);
            }

            if (isAccess && NeedsUnorderedSync(usage.state))
            {
                previous.unorderedAccess = true;
                previous.unorderedWrite = writes;
            }
        }
    }
}

void RenderGraph::Execute(FrameContext& ctx)
{
    if (!m_compiled)
        Compile();
    if (!ctx.cmdList)
        throw std::logic_error("RenderGraph: command list is missing");

    // 下一次执行需要从此轮结束后的资源状态重新计划。
    m_compiled = false;
    for (const auto& pass : m_compiledPasses)
    {
        if (!pass.barriers.empty())
            ctx.cmdList->ResourceBarrier(static_cast<UINT>(pass.barriers.size()), pass.barriers.data());
        m_passes[pass.id].execute(ctx);
    }

    for (ResourceId id = 0; id < m_resources.size(); ++id)
        m_resources[id].tracking = m_finalStates[id];
}
