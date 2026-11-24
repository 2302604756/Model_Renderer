#include "Renderer.h"
#include "Camera.h"
#include "FrameConstants.h"
#include "Model.h"
#include "PipelineBuilder.h"
#include "RootSignature.h"
#include "Scene.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

namespace {
std::string ResolveAssetPath(const char* relativePath)
{
	namespace fs = std::filesystem;
	const fs::path rel(relativePath);
	if (fs::exists(rel))
		return rel.string();
	const fs::path nested = fs::path("RenderingProgrameByHand") / rel;
	if (fs::exists(nested))
		return nested.string();

	wchar_t exeBuf[MAX_PATH] = {};
	if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH) > 0)
	{
		const fs::path exeDir = fs::path(exeBuf).parent_path();
		for (const fs::path& base : { exeDir, exeDir.parent_path(), exeDir.parent_path().parent_path() })
		{
			const fs::path c1 = base / rel;
			if (fs::exists(c1))
				return c1.string();
			const fs::path c2 = base / "RenderingProgrameByHand" / rel;
			if (fs::exists(c2))
				return c2.string();
		}
	}
	return rel.string();
}
}

Renderer::Renderer(Scene& scene) : m_scene(scene)
{
}

void Renderer::Init(HWND hwnd)
{
	// 先创建设备、交换链和 fence。后续初始化任一步失败时，也能关停已创建的上下文。
	m_context.Init(hwnd, 1920, 1080);
	m_contextReady = true;

	// 分配器依赖上下文提供的设备或交换链，必须在上下文之后初始化。
	m_cpuAllocator.Init();
	m_gpuAllocator.Init();
	m_physicalHeap.Init();
	m_uploadHeap.Init();

	ID3D12Device* device = m_context.GetDevice();

	// 复用当前帧槽的命令列表录制一次性上传和加速结构构建。
	FrameResource& frame = m_context.GetFrame();
	THROW_IF_FAILED(frame.cmdAllocator->Reset());
	THROW_IF_FAILED(frame.cmdList->Reset(frame.cmdAllocator.Get(), nullptr));

	// 深度在 G-buffer 阶段作为 DSV 写入，在 Lighting 阶段作为 SRV 读取。
	m_depthBuffer.Init(device, m_cpuAllocator, 1920, 1080);
	// 三个 G-buffer 目标分别保存反照率、编码后的世界空间法线和体积掩码。
	// 掩码清零使背景像素默认没有关联的探针体积。
	const float clearAlbedo[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	m_gbufferAlbedo.Init(device, m_cpuAllocator, m_gpuAllocator,
		1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, clearAlbedo);
	const float clearNormal[4] = { 0.5f, 0.5f, 1.0f, 1.0f };
	m_gbufferNormal.Init(device, m_cpuAllocator, m_gpuAllocator,
		1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT, clearNormal);
	const float clearVolumeMask[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	m_gbufferVolumeMask.Init(device, m_cpuAllocator, m_gpuAllocator,
		1920, 1080, DXGI_FORMAT_R32_UINT, clearVolumeMask);

	// 为同一深度资源创建 R32_FLOAT 视图，供全屏光照重建世界坐标。
	{
		GPUDescriptorHandle depthSrv = m_gpuAllocator.Allocate();
		m_depthBindlessIndex = depthSrv.index;

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		device->CreateShaderResourceView(m_depthBuffer.GetResource(), &srvDesc, depthSrv.cpuHandle);
	}

	// 把示例 DDS 上传到 GPU，并将纹理的 SRV 写到共享 bindless 堆。
	m_texture = Texture2D::LoadFromFilePlaced(
		ResolveAssetPath("assets/test.dds"),
		m_physicalHeap, m_uploadHeap, m_gpuAllocator
	);
	// 上传立方体、球和平面网格。场景使用网格 ID，光追着色通过 bindless 描述符访问其数据。
	m_cubeMeshId = m_meshLibrary.Add(kCubeVertices, kCubeIndices, m_physicalHeap, m_uploadHeap, m_gpuAllocator);

	std::vector<Vertex> sphereVertices;
	std::vector<uint32_t> sphereIndices;
	CreateSphereMesh(0.7f, 32, 16, sphereVertices, sphereIndices);
	m_sphereMeshId = m_meshLibrary.Add(sphereVertices, sphereIndices, m_physicalHeap, m_uploadHeap, m_gpuAllocator);

	std::vector<Vertex> planeVertices;
	std::vector<uint32_t> planeIndices;
	CreatePlaneMesh(9.0f, 8, planeVertices, planeIndices);
	m_planeMeshId = m_meshLibrary.Add(planeVertices, planeIndices, m_physicalHeap, m_uploadHeap, m_gpuAllocator);

	m_textureIndex = m_texture.GetBindlessIndex();
	// 场景依赖已分配的网格和纹理 ID，GPU 场景数据与 TLAS 又依赖同一批场景对象。
	m_scene.Build(m_cubeMeshId, m_sphereMeshId, m_textureIndex);
	m_tlasUpdatePass.Init(&m_rtScene, &m_scene);
	BuildLights();
	m_sceneGpuData.Build(device, m_gpuAllocator, m_meshLibrary, m_scene);
	// 在同一条命令列表中录制 BLAS/TLAS 构建，使构建顺序排在几何上传之后。
	m_rtScene.Build(device, frame.cmdList.Get(), m_meshLibrary, m_scene.GetObjects());

	// 提交完整初始化命令列表并等待 GPU 结束上传和构建。
	THROW_IF_FAILED(frame.cmdList->Close());
	ID3D12CommandList* const lists[] = { frame.cmdList.Get() };
	m_context.GetCommandQueue()->ExecuteCommandLists(1, lists);
	m_context.WaitForGPU();

	// 等待结束后才能释放初始化构建的临时资源。
	m_rtScene.ReleaseBuildScratch();
	// 为每个帧槽登记 TLAS 描述符，供探针求交和逐像素光追使用。
	m_rtScene.CreateTLASDescriptor(device, m_gpuAllocator);
	for (uint32_t slot = 0; slot < rt::AccelerationScene::kFrameCount; ++slot)
	{
		const std::string name = "TLAS[" + std::to_string(slot) + "]";
		m_tlasIds[slot] = m_graph.RegisterResource(name.c_str(), m_rtScene.GetTLASResource(slot),
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
	}

	// 重置初始化上传堆的分配游标，允许复用这段上传内存。
	m_uploadHeap.Reset();


	// GBuffer 与 Lighting 共享图形根签名。根参数槽与着色器寄存器各有自己的编号。
	m_rootSignature = RootSignatureBuilder()
		.Add32BitConstants(40, 0)    // [0] b0，逐物体参数。
		.AddRootCBV(1)               // [1] b1，相机矩阵等逐帧常量。
		.Add32BitConstants(16, 2)    // [2] b2，全屏光照参数（16 DWORD）。
		.AddRootDescriptorTable(3)   // [3] t3，TLAS 描述符表。
		.AddRootCBV(3)               // [4] b3，光照常量及探针、体积掩码索引。
		.AddRootSRV(5)               // [5] t5，光源数组。
		.AddDefaultStaticSamplers()  // s0 和 s1，静态采样器。
		.Build(device);

	// 运行时分别编译 G-buffer 和 Lighting 的 VS/PS 入口，创建对应的图形 PSO。
	auto vsObjectBlob = d3d12lite::CompileShaderSM66(L"ShadersGBuffer.hlsl", L"VSMain", L"vs_6_6");
	auto gbufferPsBlob = d3d12lite::CompileShaderSM66(L"ShadersGBuffer.hlsl", L"GBufferPS", L"ps_6_6");
	auto lightingVsBlob = d3d12lite::CompileShaderSM66(L"ShadersLighting.hlsl", L"LightingVS", L"vs_6_6");
	auto lightingPsBlob = d3d12lite::CompileShaderSM66(L"ShadersLighting.hlsl", L"LightingPS", L"ps_6_6");
	assert(!vsObjectBlob.empty() && !gbufferPsBlob.empty()
		&& !lightingVsBlob.empty() && !lightingPsBlob.empty());

	// G-buffer PSO 的三个 RTV 格式和顺序须与实际绑定以及 SV_Target 输出一致。
	const DXGI_FORMAT gbufferFormats[3] = {
		DXGI_FORMAT_R8G8B8A8_UNORM,          // albedo
		DXGI_FORMAT_R16G16B16A16_FLOAT,       // world normal
		DXGI_FORMAT_R32_UINT                 // 体积掩码
	};

	PipelineBuilder gbufferBuilder;
	m_gbufferPSO = gbufferBuilder
		.SetRootSignature(m_rootSignature.Get())
		.SetShaders(vsObjectBlob, gbufferPsBlob)
		.SetRTVFormats(gbufferFormats, 3)
		.SetDepthFormat(DepthBuffer::kFormat)
		.Build(device);

	// Lighting 用全屏三角形写交换链缓冲。深度通过 SRV 采样，因此关闭深度测试。
	const DXGI_FORMAT backbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	PipelineBuilder lightingBuilder;
	m_lightingPSO = lightingBuilder
		.SetRootSignature(m_rootSignature.Get())
		.SetShaders(lightingVsBlob, lightingPsBlob)
		.SetRTVFormats(&backbufferFormat, 1)
		.SetDepthDisabled()
		.SetCullNone()
		.Build(device);



	// 将场景探针体积交给 DDGI。Lighting 通过只读参数连接图集和描述符。
	m_ddgi.Init(device, m_gpuAllocator, m_graph, m_scene.GetProbeVolumes());
	const DDGILightingInputs ddgi = m_ddgi.GetLightingInputs();
	// 初始状态在资源创建时登记一次，RenderGraph 跨帧保留后续状态。
	m_gbufferAlbedoId = m_graph.RegisterResource(
		"GBufferAlbedo", m_gbufferAlbedo.GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_gbufferNormalId = m_graph.RegisterResource(
		"GBufferNormal", m_gbufferNormal.GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_depthId = m_graph.RegisterResource(
		"Depth", m_depthBuffer.GetResource(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	m_gbufferVolumeMaskId = m_graph.RegisterResource(
		"GBufferVolumeMask", m_gbufferVolumeMask.GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET);

	// 为每张交换链缓冲保存图资源 ID。每帧使用和上下文相同的槽位索引。
	const uint32_t frameCount = m_context.GetFrameCount();
	m_backbufferIds.resize(frameCount);
	for (uint32_t i = 0; i < frameCount; ++i)
	{
		m_backbufferIds[i] = m_graph.RegisterResource(
			"Backbuffer", m_cpuAllocator.GetBackBufferResource(i),
			D3D12_RESOURCE_STATE_PRESENT);
	}

	// Pass 借用 GPU 对象。ResourceId 用于声明读写，bindless 索引用于着色器访问。
	m_gbufferPass.Init(m_rootSignature.Get(), m_gbufferPSO.Get(),
		&m_gbufferAlbedo, &m_gbufferNormal, &m_depthBuffer,
		&m_scene, &m_meshLibrary,
		&m_gbufferVolumeMask,
		m_gbufferAlbedoId, m_gbufferNormalId, m_depthId,
		m_gbufferVolumeMaskId);

	m_lightingPass.Init(m_rootSignature.Get(), m_lightingPSO.Get(),
		&m_gbufferAlbedo, &m_gbufferNormal, &m_depthBuffer,
		m_gbufferAlbedoId, m_gbufferNormalId, m_depthId,
		m_depthBindlessIndex,
		m_computeTexture.GetBindlessIndex(),
		ddgi.filteredAtlas,
		ddgi.gridOrigin,
		ddgi.gridSpacing,
		ddgi.gridCounts.x,
		ddgi.gridCounts.y,
		ddgi.gridCounts.z,
		ddgi.filteredSrv,
		ddgi.distanceAtlas,                         // RenderGraph 中的距离矩图集。
		ddgi.distanceSrv,                           // 着色器采样距离矩的 SRV 索引。
		m_gbufferVolumeMaskId,
		m_gbufferVolumeMask.GetBindlessIndex(),
		ddgi.probeMaskSrv,
		ddgi.probeData);

	const GBufferViews views{
		{ m_gbufferAlbedoId, m_gbufferAlbedo.GetBindlessIndex() },
		{ m_gbufferNormalId, m_gbufferNormal.GetBindlessIndex() },
		{ m_depthId, m_depthBindlessIndex },
		{ m_gbufferVolumeMaskId, m_gbufferVolumeMask.GetBindlessIndex() },
	};
	m_giPipeline.Init(device, m_cpuAllocator, m_gpuAllocator, m_graph, m_ddgi, views, 1920, 1080);
}

FrameTimings Renderer::RenderFrame(const Camera& camera, float dt,
	const RenderOptions& options)
{
	// 取得可复用的帧槽，并记录 CPU 等待与重置命令列表的耗时。
	m_context.BeginFrame();
	FrameTimings timings{};
	timings.waitMs = m_context.GetLastWaitMs();
	timings.beginMs = m_context.GetLastBeginMs();
	timings.waitHit = m_context.GetLastWaitTriggered();

	// BeginFrame 已等待当前帧槽，可以更新该槽的场景数据。
	m_sceneGpuData.Update(m_context.GetFrameIndex(), m_meshLibrary, m_scene);
	// Record 计时从场景数据更新结束后开始。
	auto recordStart = std::chrono::high_resolution_clock::now();

	FrameResource& frame = m_context.GetFrame();
	ID3D12GraphicsCommandList* cmdList = frame.cmdList.Get();
	uint32_t frameIndex = m_context.GetFrameIndex();


	// 从当前交换链槽取 RTV，Lighting 会将它作为全屏输出。
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_cpuAllocator.GetBackBufferRTV(frameIndex);

	// b1 保存相机正反向变换、世界位置、时间和帧序号。
	m_context.time += dt;
	PerFrameConstants frameData{};

	DirectX::XMMATRIX view = camera.GetView();
	DirectX::XMMATRIX proj = camera.GetProj();
	DirectX::XMMATRIX viewProj = view * proj;

	// 转置后上传以匹配 HLSL 的矩阵布局。逆矩阵供深度反投影使用。
	DirectX::XMStoreFloat4x4(&frameData.viewProj,
		DirectX::XMMatrixTranspose(viewProj));
	DirectX::XMStoreFloat4x4(&frameData.invViewProj,
		DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, viewProj)));

	const DirectX::XMFLOAT3 cameraPos = camera.GetPosition();
	frameData.cameraPos = { cameraPos.x, cameraPos.y, cameraPos.z, 1.0f };
	frameData.time = m_context.time;
	frameData.frameIndex = m_frameCounter++;

	D3D12_GPU_VIRTUAL_ADDRESS cbvGpuAddr = frame.frameUpload.UploadConstants(frameData);
	// b3 在初始化的光照常量上补齐体积掩码、探针掩码和探针数据的 bindless 索引。
	const DDGILightingInputs ddgi = m_ddgi.GetLightingInputs();
	LightingConstants lightingConstants = m_lightingConstants;
	lightingConstants.volumeMaskIndex = m_gbufferVolumeMask.GetBindlessIndex();
	lightingConstants.probeMaskIndex = ddgi.probeMaskSrv;
	lightingConstants.probeDataUavIndex = ddgi.probeDataUav;
	D3D12_GPU_VIRTUAL_ADDRESS lightingCbAddr = frame.frameUpload.UploadConstants(lightingConstants);

	// t5 绑定当前帧槽上传堆里的光源数组。该内存保留到该槽的 fence 完成。
	const uint64_t lightBytes = m_gpuLights.size() * sizeof(GPULight);
	UploadBlock lightBlock = frame.frameUpload.Allocate(lightBytes, 256);
	if (!m_gpuLights.empty())
		std::memcpy(lightBlock.cpuAddress, m_gpuLights.data(), lightBytes);


	// 绑定全图共享的 bindless 堆和视口。每个图形 pass 自行绑定根签名及参数。
	ID3D12DescriptorHeap* heaps[] = { m_gpuAllocator.GetHeap() };
	cmdList->SetDescriptorHeaps(1, heaps);

	D3D12_VIEWPORT viewport = { 0.0f, 0.0f, 1920.0f, 1080.0f, 0.0f, 1.0f };
	D3D12_RECT scissorRect = { 0, 0, 1920, 1080 };
	cmdList->RSSetViewports(1, &viewport);
	cmdList->RSSetScissorRects(1, &scissorRect);
	cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// FrameContext 把本帧的命令列表、GPU 地址、输出视图和调试模式交给 pass。
	RenderGraph::FrameContext ctx{};
	ctx.frameIndex = m_context.GetFrameIndex();
	ctx.cmdList = cmdList;
	ctx.frameConstantsAddress = cbvGpuAddr;
	ctx.lightingConstantsAddress = lightingCbAddr;
	ctx.lightBufferAddress = lightBlock.gpuAddress;
	ctx.lightCount = static_cast<uint32_t>(m_gpuLights.size());
	ctx.debugMode = options.legacyDebugMode;
	ctx.backbufferRtv = rtvHandle;
	// 从同一个帧槽配对物体数据和 TLAS，使射线命中索引能找到该帧的正确实例。
	ctx.bindings.objectDataBindlessIndex =
		m_sceneGpuData.GetBindlessIndex(ctx.frameIndex);
	ctx.bindings.tlasDescriptor =
		m_rtScene.GetTLASDescriptor(ctx.frameIndex);
	ctx.bindings.tlasResource = m_tlasIds[ctx.frameIndex % rt::AccelerationScene::kFrameCount];

	m_graph.BeginFrame();

	// 当前直接光的阴影也使用 TLAS，切去屏幕空间 GI 时仍需更新它。
	m_tlasUpdatePass.Schedule(m_graph, ctx.bindings.tlasResource);
	m_graph.AddPass(m_gbufferPass);
	const RenderGraph::ResourceId backbufferId = m_backbufferIds[ctx.frameIndex];
	if (options.legacyDebugMode != 0) {
		if (LightingPass::UsesProbeAtlases(ctx.debugMode))
			m_giPipeline.AddProbeDebugUpdates(m_graph, ctx, options.resetProbeHistory);
		m_lightingPass.Schedule(m_graph, ctx, backbufferId);
	}
	else {
		m_giPipeline.AddPasses(m_graph, ctx, options, backbufferId);
	}

	// 收尾 pass 只声明状态转换，使当前后缓冲在提交到交换链前回到 PRESENT。
	m_graph.AddPass("BackbufferToPresent",
		[backbufferId](RenderGraph::PassBuilder& builder) {
			builder.Transition(backbufferId, D3D12_RESOURCE_STATE_PRESENT);
		},
		[](RenderGraph::FrameContext&) {});

	// Execute 将图声明的屏障和各 pass 的工作录入同一条命令列表。
	m_graph.Execute(ctx);

	// 这帧的探针参数已录入，推进 CPU 端用于下帧的历史序号。
	m_giPipeline.FinishFrame();

	timings.recordMs = std::chrono::duration<float, std::milli>(
		std::chrono::high_resolution_clock::now() - recordStart).count();
	timings.usingGpuUpload = frame.frameUpload.IsUsingGpuUploadHeap();

	// 在槽位推进前取得当前帧的上传堆类型，在提交后取得 CPU 侧提交与显示耗时。
	m_context.EndFrame();
	timings.executeMs = m_context.GetLastExecuteMs();
	timings.presentMs = m_context.GetLastPresentMs();
	return timings;
}

const wchar_t* Renderer::GetAdapterName() const noexcept
{
	return m_context.GetAdapterName();
}

void Renderer::Shutdown()
{
	// 停止发帧后排空 DIRECT 队列。初始化中途失败时只等待可用的上下文。
	if (m_contextReady)
	{
		m_context.WaitForGPU();
		m_contextReady = false;
	}
	// GPU 已停止使用资源。先解除图的借用，再由销毁 Renderer 的一方释放各所有者。
	m_graph = RenderGraph{};
}

void Renderer::BuildLights()
{
	// 从初始化后的场景构建两份光照数据。t5 保存全体光源，b3 保存全局光照常量。
	const std::vector<Light>& lights = m_scene.GetLights();

	m_gpuLights.clear();
	m_gpuLights.reserve(lights.size());

	for (const Light& l : lights) {
		GPULight g{};
		g.positionType = { l.position.x, l.position.y, l.position.z,
						   static_cast<float>(l.type) };

		// GPU 光源记录保存方向单位向量，并将聚光灯锥角预先转换为余弦。
		DirectX::XMFLOAT3 dir = l.direction;
		const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
		if (len > 1e-6f) { dir.x /= len; dir.y /= len; dir.z /= len; }
		g.directionRange = { dir.x, dir.y, dir.z, l.range };

		g.colorIntensity = { l.color.x, l.color.y, l.color.z, l.intensity };

		const float cosOuter = std::cos(l.outerCone);
		const float cosInner = std::cos(l.innerCone);
		g.coneParams = { cosOuter, cosInner, 0.0f, 0.0f };

		m_gpuLights.push_back(g);
	}

	// 取场景中的首个方向光作为共享太阳参数，再设置天空与环境项。
	m_lightingConstants = {};
	for (const Light& l : lights) {
		if (l.type == LightType::Directional) {
			m_lightingConstants.sunDirection = { l.direction.x, l.direction.y, l.direction.z, l.intensity };
			m_lightingConstants.sunColor = { l.color.x, l.color.y, l.color.z, 1.0f };
			break;
		}
	}
	m_lightingConstants.skyColor = { 0.2f, 0.3f, 0.5f, 1.0f };
	m_lightingConstants.ambient = { 0.05f, 0.05f, 0.05f, 1.0f };
}
