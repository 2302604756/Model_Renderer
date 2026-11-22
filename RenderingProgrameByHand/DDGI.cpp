#include "DDGI.h"
#include "PipelineBuilder.h"
#include "RootSignature.h"
#include "BindlessDescriptorHeap.h"
#include "d3d12lite.h"

void DDGI::Init(ID3D12Device* device, GPUDescriptorAllocator& descriptors, RenderGraph& graph,
	const std::vector<ProbeVolume>& volumes)
{
	// 原始辐照度图集存放时间累积结果。体积掩码匹配表面与探针，限制跨体积采样。
	m_probeGrid.Init(device, descriptors, ProbeGrid::Desc{});
	m_probeGrid.BuildMasks(device, descriptors, volumes);
	m_probeGridId = graph.RegisterResource(
		"ProbeGrid", m_probeGrid.GetResource(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, RenderGraph::ResourceContents::Preserved);

	// 距离矩图集的 R、G 保存距离一阶和二阶矩，B 保存探针有效性。
	m_probeDistanceId = graph.RegisterResource(
		"ProbeDistanceAtlas", m_probeGrid.GetDistanceResource(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, RenderGraph::ResourceContents::Preserved);

	m_probeDataId = graph.RegisterResource(
		"ProbeData", m_probeGrid.GetProbeDataResource(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, RenderGraph::ResourceContents::Preserved);

	// 滤波辐照度单独输出给 Lighting，下一帧仍使用原始图集做时间累积。
	m_probeFilteredAtlasId = graph.RegisterResource(
		"ProbeFilteredAtlas",
		m_probeGrid.GetFilteredResource(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// 共享计算根签名。构建器添加参数的先后顺序对应各 pass 的 SetComputeRoot* 槽号。
	m_probeRootSignature = RootSignatureBuilder()
		.Add32BitConstants(16, 4)   // 槽 0 对应 b4，ProbeUpdateParams 共 16 DWORD
		.Add32BitConstants(16, 2)   // 槽 1 对应 b2，LightingParams 共 16 DWORD
		.AddRootDescriptorTable(3)  // 槽 2 对应 t3，当前帧槽的 TLAS
		.AddRootCBV(3)              // 槽 3 对应 b3，LightingConstants 包含掩码与偏移的描述符索引
		.AddRootSRV(5)              // 槽 4 对应 t5，GPULight 数组
		.Build(device, /*allowInputLayout*/ false);

	const ProbeGrid::Desc& probeDesc = m_probeGrid.GetDesc();

	// 每个探针预留 kRayCount 条射线，求交、着色与图集更新在一帧内共享这批数据。
	m_probeRayBuffer.Init(device, descriptors,
		probeDesc.GetProbeCount() * ProbeRayBuffer::kRayCount);

	m_probeRayBufferId = graph.RegisterResource(
		"ProbeRayBuffer", m_probeRayBuffer.GetResource(),
		D3D12_RESOURCE_STATE_COMMON);

	// 从探针位置生成射线并查询 TLAS，将命中距离和几何数据写入射线缓冲。
	auto rayCsBlob = d3d12lite::CompileShaderSM66(
		L"ShadersProbe.hlsl", L"ProbeRayCS", L"cs_6_6");
	assert(!rayCsBlob.empty());

	PipelineBuilder rayBuilder;
	m_probeRayPSO = rayBuilder.BuildCompute(
		device, m_probeRootSignature.Get(), rayCsBlob);

	m_probeRayPass.Init(
		m_probeRootSignature.Get(), m_probeRayPSO.Get(),
		&m_probeGrid, &m_probeRayBuffer, m_probeRayBufferId, m_probeDataId);

	// 对求交结果计算辐射亮度。未命中时使用天空色，固定方向射线跳过辐照度着色。
	auto shadeCsBlob = d3d12lite::CompileShaderSM66(
		L"ShadersProbe.hlsl", L"ProbeRayShadeCS", L"cs_6_6");
	assert(!shadeCsBlob.empty());

	PipelineBuilder shadeBuilder;
	m_probeRayShadePSO = shadeBuilder.BuildCompute(
		device, m_probeRootSignature.Get(), shadeCsBlob);

	m_probeRayShadePass.Init(
		m_probeRootSignature.Get(), m_probeRayShadePSO.Get(),
		&m_probeGrid, &m_probeRayBuffer, m_probeRayBufferId);

	// 按图集方向加权累积射线辐射亮度，更新原始辐照度。
	auto probeCsBlob = d3d12lite::CompileShaderSM66(
		L"ShadersProbe.hlsl", L"ProbeTexelUpdateCS", L"cs_6_6");
	assert(!probeCsBlob.empty());

	PipelineBuilder probeBuilder;
	m_probePSO = probeBuilder.BuildCompute(
		device, m_probeRootSignature.Get(), probeCsBlob);

	// 使用同一批射线的命中距离更新距离矩，并计算探针有效性。
	auto distCsBlob = d3d12lite::CompileShaderSM66(
		L"ShadersProbe.hlsl", L"ProbeDistanceMomentsCS", L"cs_6_6");
	assert(!distCsBlob.empty());

	PipelineBuilder distBuilder;
	m_probeDistancePSO = distBuilder.BuildCompute(
		device, m_probeRootSignature.Get(), distCsBlob);

	m_probeUpdatePass.Init(
		m_probeRootSignature.Get(), m_probePSO.Get(),
		&m_probeGrid, &m_probeRayBuffer,
		m_probeGridId, m_probeRayBufferId);

	m_probeDistancePass.Init(
		m_probeRootSignature.Get(), m_probeDistancePSO.Get(),
		&m_probeGrid, &m_probeRayBuffer,
		m_probeDistanceId, m_probeRayBufferId);


	// 两种图集共享边界修补着色器。tile 步长和目标 UAV 由各 pass 分别提供。
	auto borderCsBlob = d3d12lite::CompileShaderSM66(
		L"ShadersProbe.hlsl", L"BorderFixupCS", L"cs_6_6");
	assert(!borderCsBlob.empty());

	PipelineBuilder borderBuilder;
	m_probeBorderPSO = borderBuilder.BuildCompute(
		device, m_probeRootSignature.Get(), borderCsBlob);

	// 为滤波后的辐照度 tile 补边，使八面体方向边界附近的采样可读到正确的边缘值。
	m_probeBorderFixupPass.Init(
		m_probeRootSignature.Get(),
		m_probeBorderPSO.Get(),
		m_probeFilteredAtlasId,
		2,
		probeDesc.countX,
		probeDesc.countY,
		probeDesc.countZ,
		m_probeGrid.GetFilteredUAVIndex(),
		ProbeGrid::kTileStride,
		m_probeGrid.GetAtlasWidth(),
		m_probeGrid.GetAtlasHeight());

	// 距离矩更新只写 tile 内部。先补齐边缘，后续 Fixup 才能复制完整的距离矩 tile。
	m_probeDistanceBorderPass.Init(
		m_probeRootSignature.Get(), m_probeBorderPSO.Get(),
		m_probeDistanceId, 2,
		probeDesc.countX, probeDesc.countY, probeDesc.countZ,
		m_probeGrid.GetDistanceUAVIndex(), ProbeGrid::kDistanceTileStride,
		m_probeGrid.GetDistanceAtlasWidth(), m_probeGrid.GetDistanceAtlasHeight());

	// 从掩码匹配的有效邻近探针复制图集数据，填补无效探针对应的 tile。
	auto fixupCsBlob = d3d12lite::CompileShaderSM66(
		L"ShadersProbe.hlsl", L"ProbeFixupCS", L"cs_6_6");

	m_probeFixupPSO = PipelineBuilder().BuildCompute(
		device, m_probeRootSignature.Get(), fixupCsBlob);

	m_probeFixupPass.Init(
		m_probeRootSignature.Get(),
		m_probeFixupPSO.Get(),
		&m_probeGrid,
		m_probeGridId,
		m_probeDistanceId);

	// 结合距离矩和邻近探针数据对原始辐照度做空间滤波，写入独立的滤波图集。
	auto spatialFilterBlob = d3d12lite::CompileShaderSM66(
		L"ShadersProbe.hlsl",
		L"ProbeSpatialFilterCS",
		L"cs_6_6");

	assert(!spatialFilterBlob.empty());

	m_probeSpatialFilterPSO =
		PipelineBuilder().BuildCompute(
			device,
			m_probeRootSignature.Get(),
			spatialFilterBlob);

	m_probeSpatialFilterPass.Init(
		m_probeRootSignature.Get(),
		m_probeSpatialFilterPSO.Get(),
		&m_probeGrid,
		m_probeGridId,
		m_probeFilteredAtlasId,
		m_probeDistanceId);

	// 重定位着色器根据固定方向射线更新探针偏移。当前只初始化此 pass，逐帧更新序列未启用它。
	auto relocCsBlob = d3d12lite::CompileShaderSM66(
		L"ShadersProbe.hlsl", L"ProbeRelocationCS", L"cs_6_6");
	assert(!relocCsBlob.empty());

	m_probeRelocationPSO = PipelineBuilder().BuildCompute(
		device, m_probeRootSignature.Get(), relocCsBlob);

	m_probeRelocationPass.Init(
		m_probeRootSignature.Get(),
		m_probeRelocationPSO.Get(),
		&m_probeGrid,
		&m_probeRayBuffer,
		m_probeRayBufferId,
		m_probeDataId);

}

DDGILightingInputs DDGI::GetLightingInputs() const noexcept
{
	// ResourceId 用于声明渲染图读取状态，描述符索引随常量传给着色器。
	const auto& grid = m_probeGrid.GetDesc();
	return {
		m_probeFilteredAtlasId, m_probeDistanceId, m_probeDataId, grid.origin, grid.spacing,
		{ grid.countX, grid.countY, grid.countZ },
		m_probeGrid.GetFilteredSRVIndex(), m_probeGrid.GetDistanceSRVIndex(),
		m_probeGrid.GetMaskSRVIndex(), m_probeGrid.GetProbeDataUAVIndex(),
	};
}

void DDGI::AddUpdatePasses(RenderGraph& graph, RenderGraph::ResourceId tlasId)
{
	// 求交和命中点着色都通过 TLAS 查询可见性。
	const auto readTlas = [tlasId](RenderGraph::PassBuilder& builder) {
		builder.Read(tlasId, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
	};
	graph.AddPass(m_probeRayPass, readTlas);
	graph.AddPass(m_probeRayShadePass, readTlas);
	graph.AddPass(m_probeUpdatePass);
	graph.AddPass(m_probeDistancePass);

	// 图集版本约束距离矩补边、无效探针修复、空间滤波和滤波图集补边的依赖。
	graph.AddPass(m_probeDistanceBorderPass);
	graph.AddPass(m_probeFixupPass);
	graph.AddPass(m_probeSpatialFilterPass);
	graph.AddPass(m_probeBorderFixupPass);
}

void DDGI::PrepareFrame(RenderGraph::FrameContext& frame) const noexcept
{
	// 更新着色器在探针帧号为 0 时采用完整的新样本权重，覆盖上一轮的时间累积。
	frame.probeFrameIndex = m_resetProbeHistory ? 0u : m_probeFrameIndex;
	frame.resetProbeHistory = m_resetProbeHistory;
}

void DDGI::FinishFrame() noexcept
{
	// 重置帧已按第 0 帧录制。后续从第 1 帧继续累积，并清除本次重置请求。
	if (m_resetProbeHistory) {
		m_probeFrameIndex = 1;
		m_resetProbeHistory = false;
	}
	else {
		++m_probeFrameIndex;
	}
}
