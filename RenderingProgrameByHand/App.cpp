#include "App.h"
#include "FrameConstants.h"
#include "ObjectConstants.h"
#include "InstanceData.h"
#include <cmath>
#include <cstring>


void App::init(HWND hwnd) {

	m_hwnd = hwnd; // 记录窗口句柄
	m_lastFrameTime = std::chrono::high_resolution_clock::now();

	// 1. 初始化核心上下文与各大分配器
	m_context.Init(hwnd, 1920, 1080);
	// 相机：位置 (0,1.2,-2.5)，看向原点，宽高比跟 backbuffer 一致
	m_camera.Init({ 0.0f, 1.3f, -5.5f }, { 0.0f, 1.1f, 0.0f }, 1920.0f / 1080.0f);
	m_cpuAllocator.Init();
	m_gpuAllocator.Init();
	m_physicalHeap.Init();
	m_uploadHeap.Init(); 

	ID3D12Device* device = m_context.GetDevice();

	// 2. 开启当前槽位的 CommandList 以录制初始化拷贝指令
	FrameResource& frame = m_context.GetFrame();
	THROW_IF_FAILED(frame.cmdAllocator->Reset());
	THROW_IF_FAILED(frame.cmdList->Reset(frame.cmdAllocator.Get(), nullptr));

	//===========================================================================================================
	// 
	//DSV初始化
	m_depthBuffer.Init(device,m_cpuAllocator,1920,1080);
	// G-buffer
	const float clearAlbedo[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	m_gbufferAlbedo.Init(device, m_cpuAllocator, m_gpuAllocator,
		1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, clearAlbedo);
	const float clearNormal[4] = { 0.5f, 0.5f, 1.0f, 1.0f };
	m_gbufferNormal.Init(device, m_cpuAllocator, m_gpuAllocator,
		1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT, clearNormal);
	const float clearVolumeMask[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	m_gbufferVolumeMask.Init(device, m_cpuAllocator, m_gpuAllocator,
		1920, 1080, DXGI_FORMAT_R32_UINT, clearVolumeMask);

	// depth 的 SRV：延迟光照里重建世界坐标用（D32_FLOAT -> R32_FLOAT）
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

	// 录制纹理上传指令并直写 Bindless 堆
	m_texture = Texture2D::LoadFromFilePlaced(
		R"(D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\assets\test.dds)",
		m_physicalHeap, m_uploadHeap, m_gpuAllocator
	);
	//===========================================================================================================
	 
	
	//===========================================================================================================
	// 创建几何：cube + 程序化球/平面（每个 mesh 登记到 Bindless 堆）
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
	//============================================================================================================


	// ① 场景先建好：TLAS 的实例数据来自 m_objects
	m_scene.Build(m_cubeMeshId, m_sphereMeshId, m_textureIndex);
	// Build 里固定顺序：6 = 小球，2 = 后墙
	m_demoController.Init(&m_scene, 6, 5);
	m_tlasUpdatePass.Init(&m_rtScene, &m_scene);
	BuildLights();                       
	m_sceneGpuData.Build(device, m_gpuAllocator, m_meshLibrary, m_scene);
	// ② 录制 BLAS/TLAS 构建命令（和上面的拷贝命令共用同一条命令列表）
	m_rtScene.Build(device, frame.cmdList.Get(), m_meshLibrary, m_scene.GetObjects());

	// 提交拷贝指令并等待 GPU 彻底完成搬运
	THROW_IF_FAILED(frame.cmdList->Close());
	ID3D12CommandList* const lists[] = { frame.cmdList.Get() };
	m_context.GetCommandQueue()->ExecuteCommandLists(1, lists); // 需在 RenderContext.h 暴露 GetCommandQueue()
	m_context.WaitForGPU();

	// ③ GPU 完成后才能释放构建用的临时内存
	m_rtScene.ReleaseBuildScratch();
	// ④ 注册 TLAS 的 SRV 描述符（lighting pass 里要用）
	m_rtScene.CreateTLASDescriptor(device, m_gpuAllocator);

	// 拷贝完成后，重置 UploadHeap 游标以释放上传空间
	m_uploadHeap.Reset();


	// 4. 创建 Bindless 根签名
	// App.cpp 的 App::init 中：
	m_rootSignature = RootSignatureBuilder()
		.Add32BitConstants(40, 0)    // Root Parameter [0] -> register(b0): Push Constants    索引
		.AddRootCBV(1)               // Root Parameter [1] -> register(b1): 逐帧瞬态 CBV	  传入Constant Buffer
		.Add32BitConstants(16, 2)    // [2] b2 lighting 参数（18 DWORD）
		.AddRootDescriptorTable(3)   // [3] t3 TLAS
		.AddRootCBV(3)               // [4] b3 光照常量
		.AddRootSRV(5)               // [5] t5 光源数组
		.AddDefaultStaticSamplers()  // Static Sampler    -> register(s0), register(s1)       sampler
		.Build(device);

	//==================================================================================================================
	//PSO init
	auto vsObjectBlob = d3d12lite::CompileShaderSM66(L"ShadersGBuffer.hlsl", L"VSMain", L"vs_6_6");
	auto gbufferPsBlob = d3d12lite::CompileShaderSM66(L"ShadersGBuffer.hlsl", L"GBufferPS", L"ps_6_6");
	auto lightingVsBlob = d3d12lite::CompileShaderSM66(L"ShadersLighting.hlsl", L"LightingVS", L"vs_6_6");
	auto lightingPsBlob = d3d12lite::CompileShaderSM66(L"ShadersLighting.hlsl", L"LightingPS", L"ps_6_6");
	assert(!vsObjectBlob.empty() && !gbufferPsBlob.empty()
		&& !lightingVsBlob.empty() && !lightingPsBlob.empty());

	// PSO 1：G-buffer（2 个 RTV + 深度）
	const DXGI_FORMAT gbufferFormats[3] = {
		DXGI_FORMAT_R8G8B8A8_UNORM,          // albedo
		DXGI_FORMAT_R16G16B16A16_FLOAT,       // world normal
		DXGI_FORMAT_R32_UINT
	};

	PipelineBuilder gbufferBuilder;
	m_gbufferPSO = gbufferBuilder
		.SetRootSignature(m_rootSignature.Get())
		.SetShaders(vsObjectBlob, gbufferPsBlob)
		.SetRTVFormats(gbufferFormats, 3)
		.SetDepthFormat(DepthBuffer::kFormat)
		.Build(device);

	// PSO 2：Lighting（1 个 RTV，无深度）
	const DXGI_FORMAT backbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	PipelineBuilder lightingBuilder;
	m_lightingPSO = lightingBuilder
		.SetRootSignature(m_rootSignature.Get())
		.SetShaders(lightingVsBlob, lightingPsBlob)
		.SetRTVFormats(&backbufferFormat, 1)
		.SetDepthDisabled()
		.SetCullNone()
		.Build(device);


	//==================================================================================================================
		// ---- compute 根签名 + PSO ----
	m_computeRootSignature = RootSignatureBuilder()
		.Add32BitConstants(8, 0)          // b0: ComputeParams
		.Build(device, /*allowInputLayout*/ false);

	auto computeCsBlob = d3d12lite::CompileShaderSM66(
		L"ShadersCompute.hlsl", L"ComputeSmokeCS", L"cs_6_6");
	assert(!computeCsBlob.empty());

	PipelineBuilder computeBuilder;
	m_computePSO = computeBuilder.BuildCompute(
		device, m_computeRootSignature.Get(), computeCsBlob);

	// ---- compute 输出纹理（UAV + SRV）----
	m_computeTexture.InitUAV(device, m_gpuAllocator, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM);

	m_computeTextureId = m_graph.RegisterResource(
		"ComputeTexture", m_computeTexture.GetResource(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	m_computeSmokePass.Init(
		m_computeRootSignature.Get(), m_computePSO.Get(),
		&m_computeTexture, m_computeTextureId, 1920, 1080);

	//======================================================================================================================================
		// ---- DDGI 探针网格 ----
	m_probeGrid.Init(device, m_gpuAllocator, ProbeGrid::Desc{});
	m_probeGrid.BuildMasks(device, m_gpuAllocator, m_scene.GetProbeVolumes());
	m_probeGridId = m_graph.RegisterResource(
		"ProbeGrid", m_probeGrid.GetResource(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// 距离矩 atlas（初始就是 UAV）
	m_probeDistanceId = m_graph.RegisterResource(
		"ProbeDistanceAtlas", m_probeGrid.GetDistanceResource(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// 滤波矩 
	m_probeFilteredAtlasId = m_graph.RegisterResource(
		"ProbeFilteredAtlas",
		m_probeGrid.GetFilteredResource(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// ---- probe compute 根签名 ----
	m_probeRootSignature = RootSignatureBuilder()
		.Add32BitConstants(16, 4)   // b4: ProbeUpdateParams
		.Add32BitConstants(16, 2)   // b2 LightingParams（18 DWORD）
		.AddRootDescriptorTable(3)  // t3: TLAS
		.AddRootCBV(3)              // b3: LightingConstants
		.AddRootSRV(5)              // t5: GPULight[]
		.Build(device, /*allowInputLayout*/ false);

	const ProbeGrid::Desc& probeDesc = m_probeGrid.GetDesc();

	// ---- ProbeRayBuffer ----
	m_probeRayBuffer.Init(device, m_gpuAllocator,
		probeDesc.GetProbeCount()* ProbeRayBuffer::kRayCount);

	m_probeRayBufferId = m_graph.RegisterResource(
		"ProbeRayBuffer", m_probeRayBuffer.GetResource(),
		D3D12_RESOURCE_STATE_COMMON);

	// ProbeRay PSO
	auto rayCsBlob = d3d12lite::CompileShaderSM66(
		L"ShadersProbe.hlsl", L"ProbeRayCS", L"cs_6_6");
	assert(!rayCsBlob.empty());

	PipelineBuilder rayBuilder;
	m_probeRayPSO = rayBuilder.BuildCompute(
		device, m_probeRootSignature.Get(), rayCsBlob);

	m_probeRayPass.Init(
		m_probeRootSignature.Get(), m_probeRayPSO.Get(),
		&m_probeGrid, &m_probeRayBuffer, m_probeRayBufferId);

	// ProbeRayShade PSO
	auto shadeCsBlob = d3d12lite::CompileShaderSM66(
		L"ShadersProbe.hlsl", L"ProbeRayShadeCS", L"cs_6_6");
	assert(!shadeCsBlob.empty());

	PipelineBuilder shadeBuilder;
	m_probeRayShadePSO = shadeBuilder.BuildCompute(
		device, m_probeRootSignature.Get(), shadeCsBlob);

	m_probeRayShadePass.Init(
		m_probeRootSignature.Get(), m_probeRayShadePSO.Get(),
		&m_probeGrid, &m_probeRayBuffer, m_probeRayBufferId);

	// ---- ProbeUpdate PSO ----
	auto probeCsBlob = d3d12lite::CompileShaderSM66(
		L"ShadersProbe.hlsl", L"ProbeTexelUpdateCS", L"cs_6_6");
	assert(!probeCsBlob.empty());

	PipelineBuilder probeBuilder;
	m_probePSO = probeBuilder.BuildCompute(
		device, m_probeRootSignature.Get(), probeCsBlob);

	// ---- ProbeDistance PSO ----
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


	// ---- BorderFixup PSO（两个实例共用）----
	auto borderCsBlob = d3d12lite::CompileShaderSM66(
		L"ShadersProbe.hlsl", L"BorderFixupCS", L"cs_6_6");
	assert(!borderCsBlob.empty());

	PipelineBuilder borderBuilder;
	m_probeBorderPSO = borderBuilder.BuildCompute(
		device, m_probeRootSignature.Get(), borderCsBlob);

	// irradiance 的 gutter
	m_probeBorderFixupPass.Init(
		m_probeRootSignature.Get(),
		m_probeBorderPSO.Get(),
		m_probeFilteredAtlasId,
		m_probeGrid.GetFilteredResource(),
		probeDesc.countX,
		probeDesc.countY,
		probeDesc.countZ,
		m_probeGrid.GetFilteredUAVIndex(),
		ProbeGrid::kTileStride,
		m_probeGrid.GetAtlasWidth(),
		m_probeGrid.GetAtlasHeight());

	// distance 的 gutter
	m_probeDistanceBorderPass.Init(
		m_probeRootSignature.Get(), m_probeBorderPSO.Get(),
		m_probeDistanceId, m_probeGrid.GetDistanceResource(),
		probeDesc.countX, probeDesc.countY, probeDesc.countZ,
		m_probeGrid.GetDistanceUAVIndex(), ProbeGrid::kDistanceTileStride,
		m_probeGrid.GetDistanceAtlasWidth(), m_probeGrid.GetDistanceAtlasHeight());

	// ---- ProbeFixupCS PSO -----
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

	// ---- ProbeFilterPass
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
		m_probeGrid.GetFilteredResource(),
		m_probeGridId,
		m_probeFilteredAtlasId,
		m_probeDistanceId);

	// ---- ProbeRelocation PSO（帧末更新下帧使用的探针偏移）----
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
		m_probeRayBufferId);

	//==================================================================================================================
	// ---- RenderGraph：资源注册（只注册一次，状态跨帧保留）----
	m_gbufferAlbedoId = m_graph.RegisterResource(
		"GBufferAlbedo", m_gbufferAlbedo.GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_gbufferNormalId = m_graph.RegisterResource(
		"GBufferNormal", m_gbufferNormal.GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_depthId = m_graph.RegisterResource(
		"Depth", m_depthBuffer.GetResource(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	m_gbufferVolumeMaskId = m_graph.RegisterResource(
		"GBufferVolumeMask",m_gbufferVolumeMask.GetResource(),D3D12_RESOURCE_STATE_RENDER_TARGET);

	const uint32_t frameCount = m_context.GetFrameCount();
	m_backbufferIds.resize(frameCount);
	for (uint32_t i = 0; i < frameCount; ++i)
	{
		m_backbufferIds[i] = m_graph.RegisterResource(
			"Backbuffer", m_cpuAllocator.GetBackBufferResource(i),
			D3D12_RESOURCE_STATE_PRESENT);
	}

	// ---- Pass 初始化 ----
	m_gbufferPass.Init(m_gbufferPSO.Get(),
		&m_gbufferAlbedo, &m_gbufferNormal, &m_depthBuffer,
		&m_scene, &m_meshLibrary,
		&m_gbufferVolumeMask,
		m_gbufferAlbedoId, m_gbufferNormalId, m_depthId,
		m_gbufferVolumeMaskId);

	m_lightingPass.Init(m_lightingPSO.Get(),
		&m_gbufferAlbedo, &m_gbufferNormal, &m_depthBuffer,
		m_gbufferAlbedoId, m_gbufferNormalId, m_depthId,
		m_depthBindlessIndex,
		m_computeTexture.GetBindlessIndex(),
		m_probeFilteredAtlasId,
		m_probeGrid.GetDesc().origin,
		m_probeGrid.GetDesc().spacing,
		m_probeGrid.GetDesc().countX,
		m_probeGrid.GetDesc().countY,
		m_probeGrid.GetDesc().countZ,
		m_probeGrid.GetFilteredSRVIndex(),
		m_probeDistanceId,                          // 距离矩资源 id
		m_probeGrid.GetDistanceSRVIndex(),			// 距离矩 SRV 槽
		m_gbufferVolumeMaskId,
		m_gbufferVolumeMask.GetBindlessIndex(),
		m_probeGrid.GetMaskSRVIndex());         

}

void App::RenderTick() {
	auto tickStart = std::chrono::high_resolution_clock::now();
	// 0. 真实帧间隔 dt（并 clamp，卡顿/断点后不会瞬移）
	auto now = std::chrono::high_resolution_clock::now();
	float dt = std::chrono::duration<float>(now - m_lastFrameTime).count();
	m_lastFrameTime = now;
	if (dt > 0.1f) dt = 0.1f;

	UpdateFrameStats(dt);

	// 相机更新：先消费鼠标增量，再把手里的增量清零
	m_camera.Update(dt, BuildCameraControl());
	m_input.ResetMouseDelta();

	//用于切换视图
	//我们通过修改传入shader的参数来控制输出内容来验证延迟渲染的执行正确性
	if (m_input.IsKeyDown('1')) m_debugMode = 0;   // final DDGI
	if (m_input.IsKeyDown('2')) m_debugMode = 16;  // final RT GI
	if (m_input.IsKeyDown('3')) m_debugMode = 17;  // DDGI only
	if (m_input.IsKeyDown('4')) m_debugMode = 18;  // RT GI only
	if (m_input.IsKeyDown('N')) m_debugMode = 2;   // normal
	if (m_input.IsKeyDown('5')) m_debugMode = 11;  // 有效探针数
	if (m_input.IsKeyDown('6')) m_debugMode = 12;  // Chebyshev 可见性
	if (m_input.IsKeyDown('7')) m_debugMode = 13;  // 总权重
	if (m_input.IsKeyDown('8')) m_debugMode = 14;  // 主导探针
	if (m_input.IsKeyDown('9')) m_debugMode = 8;   // 探针辐照度
	if (m_input.IsKeyDown('0')) m_debugMode = 9;   // atlas 布局
	if (m_input.IsKeyDown('P')) m_debugMode = 10;  // 距离矩 atlas
	if (m_input.IsKeyDown('T')) m_debugMode = 15;  // 
	if (m_input.IsKeyDown('V')) m_debugMode = 19;  // 探针位置
	if (m_input.IsKeyDown('R')) m_resetProbeHistory = true;	// 重新收敛探针位置

	// 先 BeginFrame：它会等待当前帧槽位 3 帧前的 fence
	m_context.BeginFrame();
	m_waitMsAccum += m_context.GetLastWaitMs();
	m_beginMsAccum += m_context.GetLastBeginMs();
	m_waitHitCount += m_context.GetLastWaitTriggered();

	// 现在 CPU 可以安全地写“当前帧槽位”
	m_demoController.Update(dt, m_input);
	m_sceneGpuData.Update(m_context.GetFrameIndex(), m_meshLibrary, m_scene);		//新增根据FrameIndex来Update
	auto recordStart = std::chrono::high_resolution_clock::now();

	//======================================================================================================
	//下面是渲染pass主体

	FrameResource& frame = m_context.GetFrame();
	ID3D12GraphicsCommandList* cmdList = frame.cmdList.Get();
	uint32_t frameIndex = m_context.GetFrameIndex();


	// 1. 从 CPU 分配器获取当前后缓冲资源与 RTV,DSV 句柄
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_cpuAllocator.GetBackBufferRTV(frameIndex);
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_depthBuffer.GetDSV();

	// ---- 每帧常量 ----
	m_context.time += dt;
	PerFrameConstants frameData{};

	DirectX::XMMATRIX view = m_camera.GetView();
	DirectX::XMMATRIX proj = m_camera.GetProj();
	DirectX::XMMATRIX viewProj = view * proj;

	DirectX::XMStoreFloat4x4(&frameData.viewProj,
		DirectX::XMMatrixTranspose(viewProj));
	DirectX::XMStoreFloat4x4(&frameData.invViewProj,
		DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, viewProj)));

	const DirectX::XMFLOAT3 cameraPos = m_camera.GetPosition();
	frameData.cameraPos = { cameraPos.x, cameraPos.y, cameraPos.z, 1.0f };
	frameData.time = m_context.time;
	frameData.frameIndex = m_frameCounter++;

	D3D12_GPU_VIRTUAL_ADDRESS cbvGpuAddr = frame.frameUpload.UploadConstants(frameData);
	// ---- 光照常量（b3）----
	LightingConstants lightingConstants = m_lightingConstants;
	// 每帧解析 mask SRV，放进 b3，不占 root signature DWORD
	lightingConstants.volumeMaskIndex = m_gbufferVolumeMask.GetBindlessIndex();
	lightingConstants.probeMaskIndex = m_probeGrid.GetMaskSRVIndex();
	// 每帧
	lightingConstants.probeDataUavIndex = m_probeGrid.GetProbeDataUAVIndex();
	D3D12_GPU_VIRTUAL_ADDRESS lightingCbAddr = frame.frameUpload.UploadConstants(lightingConstants);

	// ---- 光源数组（t5）：每帧写进 frameUpload ----
	const uint64_t lightBytes = m_gpuLights.size() * sizeof(GPULight);
	UploadBlock lightBlock = frame.frameUpload.Allocate(lightBytes, 256);
	if (!m_gpuLights.empty())
		std::memcpy(lightBlock.cpuAddress, m_gpuLights.data(), lightBytes);


	// ---- 通用状态 ----
	ID3D12DescriptorHeap* heaps[] = { m_gpuAllocator.GetHeap() };
	cmdList->SetDescriptorHeaps(1, heaps);
	cmdList->SetGraphicsRootSignature(m_rootSignature.Get());

	D3D12_VIEWPORT viewport = { 0.0f, 0.0f, 1920.0f, 1080.0f, 0.0f, 1.0f };
	D3D12_RECT scissorRect = { 0, 0, 1920, 1080 };
	cmdList->RSSetViewports(1, &viewport);
	cmdList->RSSetScissorRects(1, &scissorRect);
	cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmdList->SetGraphicsRootConstantBufferView(1, cbvGpuAddr);			 //每帧常量

	//Render Graph调用
	RenderGraph::FrameContext ctx{};
	ctx.frameIndex = m_context.GetFrameIndex();
	ctx.cmdList = cmdList;
	ctx.frameConstantsAddress = cbvGpuAddr;
	ctx.lightingConstantsAddress = lightingCbAddr;
	ctx.lightBufferAddress = lightBlock.gpuAddress;
	ctx.lightCount = static_cast<uint32_t>(m_gpuLights.size());
	ctx.debugMode = m_debugMode;
	ctx.backbufferRtv = rtvHandle;
	ctx.probeFrameIndex = m_resetProbeHistory ? 0u : m_probeFrameIndex;
	ctx.resetProbeHistory = m_resetProbeHistory;
	// App 是组合根：每帧解析一次 bindings，Pass 只读 ctx.bindings
	// 每帧解析一次，Pass 不再关心数据来自哪个模块
	ctx.bindings.objectDataBindlessIndex =
		m_sceneGpuData.GetBindlessIndex(ctx.frameIndex);
	ctx.bindings.tlasDescriptor =
		m_rtScene.GetTLASDescriptor(ctx.frameIndex);

	m_lightingPass.SetBackbuffer(m_backbufferIds[ctx.frameIndex]);

	m_graph.BeginFrame();

	m_graph.AddPass(m_tlasUpdatePass);   // 新：每帧重建 TLAS
	m_graph.AddPass(m_gbufferPass);
	m_graph.AddPass(m_computeSmokePass);
	m_graph.AddPass(m_probeRayPass);          // 生成射线 + 求交
	m_graph.AddPass(m_probeRayShadePass);     // 写 radiance
	m_graph.AddPass(m_probeUpdatePass);       // 旧的 atlas 更新，暂时保留

	m_graph.AddPass(m_probeDistancePass);         // 距离矩更新
	m_graph.AddPass(m_probeDistanceBorderPass);   // 距离矩 gutter
	m_graph.AddPass(m_probeFixupPass);			// 物体内Probe修正
	m_graph.AddPass(m_probeSpatialFilterPass);
	m_graph.AddPass(m_probeBorderFixupPass);  // irradiance gutter
	m_graph.AddPass(m_lightingPass);
	// 帧末 relocation：本帧所有 pass 用旧 offset，写入的新 offset 下一帧生效
	//m_graph.AddPass(m_probeRelocationPass);
	

	// 收尾：backbuffer 切回 PRESENT（空执行，只用于插屏障）
	const RenderGraph::ResourceId backbufferId = m_backbufferIds[ctx.frameIndex];
	m_graph.AddPass("BackbufferToPresent",
		[backbufferId](RenderGraph::PassBuilder& builder) {
			builder.Transition(backbufferId, D3D12_RESOURCE_STATE_PRESENT);
		},
		[](RenderGraph::FrameContext&) {});

	m_graph.Execute(ctx);

	if (m_resetProbeHistory) {
		m_probeFrameIndex = 1;
		m_resetProbeHistory = false;
	}
	else {
		++m_probeFrameIndex;
	}
	
	//=================================================================================================================================
	auto recordEnd = std::chrono::high_resolution_clock::now();
	const float recordMs = std::chrono::duration<float, std::milli>(recordEnd - recordStart).count();
	m_recordCpuMsAccum += recordMs;
	++m_recordCpuSamples;

	m_context.EndFrame();

	auto tickEnd = std::chrono::high_resolution_clock::now();
	m_tickCpuMsAccum += std::chrono::duration<float, std::milli>(tickEnd - tickStart).count();
	m_executeMsAccum += m_context.GetLastExecuteMs();
	m_presentMsAccum += m_context.GetLastPresentMs();



}


//边缘性系统：与主体渲染管线关系不大
void App::UpdateFrameStats(float deltaTime) {
	m_fpsFrameCount++;
	m_fpsTimeElapsed += deltaTime;

	if (m_fpsTimeElapsed >= 0.25f) {
		float avgFps = static_cast<float>(m_fpsFrameCount) / m_fpsTimeElapsed;
		float avgMs = (m_fpsTimeElapsed * 1000.0f) / static_cast<float>(m_fpsFrameCount);

		const float samples = static_cast<float>(m_recordCpuSamples);
		const float invSamples = (samples > 0.0f) ? (1.0f / samples) : 0.0f;

		const float avgWaitMs = m_waitMsAccum * invSamples;
		const float avgBeginMs = m_beginMsAccum * invSamples;
		const float avgRecordMs = m_recordCpuMsAccum * invSamples;
		const float avgExecuteMs = m_executeMsAccum * invSamples;
		const float avgPresentMs = m_presentMsAccum * invSamples;
		const float avgTickMs = m_tickCpuMsAccum * invSamples;

		wchar_t titleBuffer[1024];   // 增加 GPU 名称后扩大，避免截断
		swprintf_s(
			titleBuffer,
			L"... | GPU: %ls | FPS: %.1f | Frame: %.3f ms"
			L" | WaitGPU: %.2f ms | WaitHit: %u/%u"
			L" | Begin: %.2f ms"
			L" | Record: %.2f ms | Execute: %.2f ms | Present: %.2f ms"
			L" | TickCPU: %.2f ms | Objects: %u | Heap: %s",
			m_context.GetAdapterName(),               // 1  实际使用的显卡
			avgFps,                                   // 2  FPS
			avgMs,                                    // 3  Frame
			avgWaitMs,                                // 4  WaitGPU
			static_cast<unsigned>(m_waitHitCount),    // 5  WaitHit 分子
			static_cast<unsigned>(m_recordCpuSamples),// 6  WaitHit 分母
			avgBeginMs,                               // 7  Begin
			avgRecordMs,                              // 8  Record
			avgExecuteMs,                             // 9  Execute
			avgPresentMs,                             // 10 Present
			avgTickMs,                                // 11 TickCPU
			static_cast<unsigned>(m_scene.GetObjects().size()), // 12 Objects
			m_context.GetFrame().frameUpload.IsUsingGpuUploadHeap()
			? L"GPU_UPLOAD (ReBAR)" : L"UPLOAD (SysRAM)"); // 13 Heap

		SetWindowTextW(m_hwnd, titleBuffer);

		// 重置统计窗口（新增的累加器也要一起清）
		m_fpsFrameCount = 0;
		m_fpsTimeElapsed = 0.0f;
		m_recordCpuSamples = 0;
		m_recordCpuMsAccum = 0.0f;
		m_waitMsAccum = 0.0f;
		m_beginMsAccum = 0.0f;
		m_executeMsAccum = 0.0f;
		m_presentMsAccum = 0.0f;
		m_tickCpuMsAccum = 0.0f;
		m_waitHitCount = 0;
	}
}

CameraControl App::BuildCameraControl() const {
	CameraControl c;
	c.moveForward = (m_input.IsKeyDown('W') ? 1.0f : 0.0f) - (m_input.IsKeyDown('S') ? 1.0f : 0.0f);
	c.moveRight = (m_input.IsKeyDown('D') ? 1.0f : 0.0f) - (m_input.IsKeyDown('A') ? 1.0f : 0.0f);
	c.moveUp = (m_input.IsKeyDown('E') ? 1.0f : 0.0f) - (m_input.IsKeyDown('Q') ? 1.0f : 0.0f);
	c.lookDeltaX = m_input.GetMouseDeltaX();
	c.lookDeltaY = m_input.GetMouseDeltaY();
	return c;
}



void App::BuildLights()
{
	const std::vector<Light>& lights = m_scene.GetLights();

	m_gpuLights.clear();
	m_gpuLights.reserve(lights.size());

	for (const Light& l : lights) {
		GPULight g{};
		g.positionType = { l.position.x, l.position.y, l.position.z,
						   static_cast<float>(l.type) };

		// 方向统一归一化
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

	// 主方向光 → LightingConstants（太阳/天空/环境）
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
