#pragma once

#include <vector>
#include <wrl/client.h>
#include "ProbeGrid.h"
#include "ProbeRayBuffer.h"
#include "ProbeRayPass.h"
#include "ProbeRayShadePass.h"
#include "ProbeUpdatePass.h"
#include "ProbeDistancePass.h"
#include "ProbeBorderFixupPass.h"
#include "ProbeFixupPass.h"
#include "ProbeRelocationPass.h"
#include "ProbeSpatialFilterPass.h"

class GPUDescriptorAllocator;

// 光照阶段借用的 DDGI 图集与网格参数。返回这些值不会转移 GPU 资源所有权。
struct DDGILightingInputs {
    RenderGraph::ResourceId filteredAtlas; // 图中滤波辐照度图集的 ID，用于声明读取状态
    RenderGraph::ResourceId distanceAtlas; // 图中距离矩图集的 ID，用于声明读取状态
    DirectX::XMFLOAT3 gridOrigin;           // 网格原点的世界坐标
    float gridSpacing;                     // 相邻格点的世界空间间距
    DirectX::XMUINT3 gridCounts;            // X、Y、Z 三轴上的探针数量
    uint32_t filteredSrv;                  // b2 中供着色器采样的辐照度图集 SRV 槽
    uint32_t distanceSrv;                  // b2 中供着色器采样的距离矩图集 SRV 槽
    uint32_t probeMaskSrv;                 // b3 中用于匹配探针与表面体积掩码的 SRV 槽
    uint32_t probeDataUav;                 // b3 中保存每个探针重定位偏移的 UAV 槽
};

// Renderer 的 DDGI 子模块，持有探针资源、根签名、PSO 和更新 pass。
// RenderGraph 按引用登记资源和 pass。Renderer 等待 GPU 并清空图后才能销毁本模块。
class DDGI {
public:
    DDGI() = default;
    DDGI(const DDGI&) = delete;
    DDGI& operator=(const DDGI&) = delete;

    // 初始化探针资源，将其注册到后续逐帧使用的同一个渲染图。
    // volumes 用于生成网格的体积掩码，描述符从 Renderer 提供的全局堆分配。
    void Init(ID3D12Device* device, GPUDescriptorAllocator& descriptors, RenderGraph& graph,
        const std::vector<ProbeVolume>& volumes);

    // 初始化后调用。ResourceId 供渲染图使用，SRV/UAV 槽供着色器访问描述符堆。
    DDGILightingInputs GetLightingInputs() const noexcept;

    // 在本帧 TLAS 更新之后、Lighting 之前登记探针 pass，按顺序生成光照所需的图集。
    void AddUpdatePasses(RenderGraph& graph);

    // 请求下一次 PrepareFrame 使用探针帧号 0，重启时间累积。
    void RequestHistoryReset() noexcept { m_resetProbeHistory = true; }

    // 将当前探针帧号和重置标志写入 FrameContext，供本帧的探针 pass 读取。
    void PrepareFrame(RenderGraph::FrameContext& frame) const noexcept;

    // 在渲染图完成本帧命令录制后推进探针帧号，并消费已处理的重置请求。
    void FinishFrame() noexcept;

private:
    // DDGI 时间累积使用自己的帧号，独立于 Renderer 递增的全局帧号和交换链槽位。
    uint32_t m_probeFrameIndex = 0;
    bool m_resetProbeHistory = false;

    // 网格持有原始辐照度、距离矩、滤波辐照度、探针掩码和偏移。射线缓冲保存求交与着色的中间结果。
    ProbeGrid m_probeGrid;
    ProbeRayBuffer m_probeRayBuffer;
    RenderGraph::ResourceId m_probeGridId = RenderGraph::kInvalidId; // 原始辐照度图集
    RenderGraph::ResourceId m_probeDistanceId = RenderGraph::kInvalidId;
    RenderGraph::ResourceId m_probeFilteredAtlasId = RenderGraph::kInvalidId;
    RenderGraph::ResourceId m_probeRayBufferId = RenderGraph::kInvalidId;

    // 各 compute pass 共享根签名，由各自的 PSO 选择着色器入口。两种图集边界修补共享一个 PSO。
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_probeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_probeRayPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_probeRayShadePSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_probePSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_probeDistancePSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_probeBorderPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_probeFixupPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_probeSpatialFilterPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_probeRelocationPSO;

    // pass 借用上面的 GPU 对象，成员顺序使 pass 在其依赖的资源和 PSO 之前析构。
    ProbeRayPass m_probeRayPass;
    ProbeRayShadePass m_probeRayShadePass;
    ProbeUpdatePass m_probeUpdatePass;
    ProbeDistancePass m_probeDistancePass;
    ProbeBorderFixupPass m_probeDistanceBorderPass;
    ProbeFixupPass m_probeFixupPass;
    ProbeSpatialFilterPass m_probeSpatialFilterPass;
    ProbeBorderFixupPass m_probeBorderFixupPass;
    ProbeRelocationPass m_probeRelocationPass; // 已初始化，当前更新序列未登记此 pass
};
