#include "App.h"
#include <cmath>


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
	BuildScene();
	BuildInstances();
	// ② 录制 BLAS/TLAS 构建命令（和上面的拷贝命令共用同一条命令列表）
	m_rtScene.Build(device, frame.cmdList.Get(), m_meshLibrary, m_objects);

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
		.Add32BitConstants(8, 2)     // b2: lighting pass 参数
		.AddRootDescriptorTable(3)   // [3] t3 TLAS
		.AddDefaultStaticSamplers()  // Static Sampler    -> register(s0), register(s1)       sampler
		.Build(device);

	//==================================================================================================================
	//PSO init
	auto vsObjectBlob = d3d12lite::CompileShaderSM66(L"Shaders.hlsl", L"VSMain", L"vs_6_6");
	auto gbufferPsBlob = d3d12lite::CompileShaderSM66(L"Shaders.hlsl", L"GBufferPS", L"ps_6_6");
	auto lightingVsBlob = d3d12lite::CompileShaderSM66(L"Shaders.hlsl", L"LightingVS", L"vs_6_6");
	auto lightingPsBlob = d3d12lite::CompileShaderSM66(L"Shaders.hlsl", L"LightingPS", L"ps_6_6");
	assert(!vsObjectBlob.empty() && !gbufferPsBlob.empty()
		&& !lightingVsBlob.empty() && !lightingPsBlob.empty());

	// PSO 1：G-buffer（2 个 RTV + 深度）
	const DXGI_FORMAT gbufferFormats[2] = {
		DXGI_FORMAT_R8G8B8A8_UNORM,          // albedo
		DXGI_FORMAT_R16G16B16A16_FLOAT       // world normal
	};

	PipelineBuilder gbufferBuilder;
	m_gbufferPSO = gbufferBuilder
		.SetRootSignature(m_rootSignature.Get())
		.SetShaders(vsObjectBlob, gbufferPsBlob)
		.SetRTVFormats(gbufferFormats, 2)
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
	if (m_input.IsKeyDown('1')) m_debugMode = 0;
	if (m_input.IsKeyDown('2')) m_debugMode = 1;   // albedo
	if (m_input.IsKeyDown('3')) m_debugMode = 2;   // normal
	if (m_input.IsKeyDown('4')) m_debugMode = 3;   // depth
	if (m_input.IsKeyDown('5')) m_debugMode = 4;   // world pos
	if (m_input.IsKeyDown('6')) m_debugMode = 5;   // ray-tracing
	if (m_input.IsKeyDown('7')) m_debugMode = 6;   // GI only

	m_context.BeginFrame();
	m_waitMsAccum += m_context.GetLastWaitMs();
	m_beginMsAccum += m_context.GetLastBeginMs();
	m_waitHitCount += m_context.GetLastWaitTriggered();
	auto recordStart = std::chrono::high_resolution_clock::now();

	//======================================================================================================
	//下面是渲染pass主体

	FrameResource& frame = m_context.GetFrame();
	ID3D12GraphicsCommandList* cmdList = frame.cmdList.Get();
	uint32_t frameIndex = m_context.GetFrameIndex();


	// 1. 从 CPU 分配器获取当前后缓冲资源与 RTV,DSV 句柄
	ID3D12Resource* backBuffer = m_cpuAllocator.GetBackBufferResource(frameIndex);
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

	D3D12_GPU_VIRTUAL_ADDRESS cbvGpuAddr = frame.frameUpload.UploadConstants(frameData);

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


	// ================= Pass 1: G-buffer =================
	m_gbufferAlbedo.TransitionTo(cmdList, D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_gbufferNormal.TransitionTo(cmdList, D3D12_RESOURCE_STATE_RENDER_TARGET);
	TransitionDepth(cmdList, D3D12_RESOURCE_STATE_DEPTH_WRITE);

	D3D12_CPU_DESCRIPTOR_HANDLE gbufferRtvs[2] = {
		m_gbufferAlbedo.GetRTV(), m_gbufferNormal.GetRTV() };
	cmdList->OMSetRenderTargets(2, gbufferRtvs, FALSE, &dsv);

	const float clearAlbedo[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	const float clearNormal[4] = { 0.5f, 0.5f, 1.0f, 1.0f };
	cmdList->ClearRenderTargetView(gbufferRtvs[0], clearAlbedo, 0, nullptr);
	cmdList->ClearRenderTargetView(gbufferRtvs[1], clearNormal, 0, nullptr);
	m_depthBuffer.Clear(cmdList);

	cmdList->SetPipelineState(m_gbufferPSO.Get());

	for (const RenderObject& obj : m_objects) {
		const DirectX::XMMATRIX world =
			DirectX::XMMatrixScaling(obj.scale.x, obj.scale.y, obj.scale.z) *
			DirectX::XMMatrixRotationRollPitchYaw(obj.rotation.x, obj.rotation.y, obj.rotation.z) *
			DirectX::XMMatrixTranslation(obj.position.x, obj.position.y, obj.position.z);

		PerObjectConstants objectData{};
		DirectX::XMStoreFloat4x4(&objectData.world, DirectX::XMMatrixTranspose(world));
		DirectX::XMStoreFloat4x4(&objectData.normalMatrix, DirectX::XMMatrixInverse(nullptr, world));

		const Mesh& mesh = m_meshLibrary.Get(obj.meshId);
		objectData.vbIndex = mesh.GetVBBindlessIndex();
		objectData.textureIndex = obj.textureIndex;
		objectData.albedo = obj.albedo;

		cmdList->SetGraphicsRoot32BitConstants(0, 40, &objectData, 0);		//每物体常量
		cmdList->IASetIndexBuffer(&mesh.GetIBV());
		cmdList->DrawIndexedInstanced(mesh.GetIndexCount(), 1, 0, 0, 0);
	}

	// ================= 屏障：G-buffer -> SRV =================
	m_gbufferAlbedo.TransitionTo(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	m_gbufferNormal.TransitionTo(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	TransitionDepth(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	D3D12_RESOURCE_BARRIER barrierBefore = d3d12lite::CreateTransitionBarrier(
		backBuffer,
		D3D12_RESOURCE_STATE_PRESENT,
		D3D12_RESOURCE_STATE_RENDER_TARGET);
	cmdList->ResourceBarrier(1, &barrierBefore);

	// ================= Pass 2: Lighting =================
	cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
	const float clearColor[4] = { 0.1f, 0.1f, 0.15f, 1.0f };
	cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

	cmdList->SetPipelineState(m_lightingPSO.Get());

	LightingParams lp{};
	lp.albedoIndex = m_gbufferAlbedo.GetBindlessIndex();
	lp.normalIndex = m_gbufferNormal.GetBindlessIndex();
	lp.depthIndex = m_depthBindlessIndex;
	lp.debugMode = m_debugMode;
	lp.objectIndex = m_objectDataBindlessIndex;
	cmdList->SetGraphicsRoot32BitConstants(2, 8, &lp, 0);
	cmdList->SetGraphicsRootDescriptorTable(3, m_rtScene.GetTLASDescriptor());
	cmdList->DrawInstanced(3, 1, 0, 0);

	// ================= 收尾 =================
	D3D12_RESOURCE_BARRIER barrierAfter = d3d12lite::CreateTransitionBarrier(
		backBuffer,
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PRESENT);
	cmdList->ResourceBarrier(1, &barrierAfter);

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

		wchar_t titleBuffer[512];   // 字段变多了，256 可能不够
		swprintf_s(
			titleBuffer,
			L"... | FPS: %.1f | Frame: %.3f ms"
			L" | WaitGPU: %.2f ms | WaitHit: %u/%u"
			L" | Begin: %.2f ms"
			L" | Record: %.2f ms | Execute: %.2f ms | Present: %.2f ms"
			L" | TickCPU: %.2f ms | Objects: %u | Heap: %s",
			avgFps,                                  // 1  FPS
			avgMs,                                   // 2  Frame
			avgWaitMs,                               // 3  WaitGPU
			static_cast<unsigned>(m_waitHitCount),   // 4  WaitHit 分子
			static_cast<unsigned>(m_recordCpuSamples), // 5  WaitHit 分母
			avgBeginMs,                              // 6  Begin
			avgRecordMs,                             // 7  Record
			avgExecuteMs,                            // 8  Execute
			avgPresentMs,                            // 9  Present
			avgTickMs,                               // 10 TickCPU
			static_cast<unsigned>(m_objects.size()), // 11 Objects
			m_context.GetFrame().frameUpload.IsUsingGpuUploadHeap()
			? L"GPU_UPLOAD (ReBAR)" : L"UPLOAD (SysRAM)"); // 12 Heap

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

// 量测用：14 / 100 / 1000 / 5000 切换
constexpr uint32_t kTestObjectCount = 10000;

void App::BuildScene() {
	m_objects.clear();

	auto addObject = [this](const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& scale,
		uint32_t meshId, const DirectX::XMFLOAT4& albedo) {
			RenderObject obj{};
			obj.position = pos;
			obj.scale = scale;
			obj.rotation = { 0.0f, 0.0f, 0.0f };
			obj.meshId = meshId;
			obj.textureIndex = m_textureIndex;
			obj.albedo = albedo;
			m_objects.push_back(obj);
		};

	const DirectX::XMFLOAT4 white{ 0.80f, 0.80f, 0.80f, 1.0f };
	const DirectX::XMFLOAT4 red{ 0.85f, 0.10f, 0.10f, 1.0f };
	const DirectX::XMFLOAT4 green{ 0.10f, 0.75f, 0.10f, 1.0f };
	const DirectX::XMFLOAT4 orange{ 0.95f, 0.55f, 0.15f, 1.0f };

	// 房间：宽 5(x)、高 2.5(y)、深 5(z)，正面(z=-2.5)敞开
	// 用薄 cube 当墙，避免平面朝向/背面剔除问题
	addObject({ 0.0f, -0.05f, 0.0f }, { 5.0f, 0.1f, 5.0f }, m_cubeMeshId, white);  // 地板
	addObject({ 0.0f,  2.55f, 0.0f }, { 5.0f, 0.1f, 5.0f }, m_cubeMeshId, white);  // 天花板
	addObject({ 0.0f,  1.25f, 2.55f }, { 5.0f, 2.6f, 0.1f }, m_cubeMeshId, white); // 后墙
	addObject({ -2.55f, 1.25f, 0.0f }, { 0.1f, 2.6f, 5.0f }, m_cubeMeshId, red);   // 左墙
	addObject({ 2.55f, 1.25f, 0.0f }, { 0.1f, 2.6f, 5.0f }, m_cubeMeshId, green); // 右墙

	// 两个球
	addObject({ -0.8f, 0.70f, -0.6f }, { 1.0f, 1.0f, 1.0f }, m_sphereMeshId, white);
	addObject({ 0.9f, 0.46f,  0.3f }, { 0.65f, 0.65f, 0.65f }, m_sphereMeshId, orange);
}

//也可以用UPLOAD_GPU优化
void App::BuildInstances() {
	// ---- 每个实例的 GPU 数据（UPLOAD 堆，一次写入，常驻）----
	ID3D12Device* device = m_context.GetDevice();
	std::vector<GPUObjectData> gpuObjects(m_objects.size());
	for (size_t i = 0; i < m_objects.size(); ++i) {
		const RenderObject& obj = m_objects[i];
		const DirectX::XMMATRIX world =
			DirectX::XMMatrixScaling(obj.scale.x, obj.scale.y, obj.scale.z) *
			DirectX::XMMatrixRotationRollPitchYaw(obj.rotation.x, obj.rotation.y, obj.rotation.z) *
			DirectX::XMMatrixTranslation(obj.position.x, obj.position.y, obj.position.z);

		GPUObjectData& o = gpuObjects[i];
		DirectX::XMStoreFloat4x4(&o.world, DirectX::XMMatrixTranspose(world));
		DirectX::XMStoreFloat4x4(&o.normalMatrix, DirectX::XMMatrixInverse(nullptr, world));
		o.albedo = obj.albedo;

		const Mesh& mesh = m_meshLibrary.Get(obj.meshId);
		o.vbIndex = mesh.GetVBBindlessIndex();
		o.ibIndex = mesh.GetIBBindlessIndex();
		o.indexCount = mesh.GetIndexCount();
		o.padding = 0;
	}

	const uint64_t objBytes = gpuObjects.size() * sizeof(GPUObjectData);
	D3D12_RESOURCE_DESC objDesc{};
	objDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	objDesc.Width = objBytes;
	objDesc.Height = 1;
	objDesc.DepthOrArraySize = 1;
	objDesc.MipLevels = 1;
	objDesc.Format = DXGI_FORMAT_UNKNOWN;
	objDesc.SampleDesc = { 1, 0 };
	objDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	D3D12_HEAP_PROPERTIES uploadProps{};
	uploadProps.Type = D3D12_HEAP_TYPE_GPU_UPLOAD;
	THROW_IF_FAILED(device->CreateCommittedResource(
		&uploadProps, D3D12_HEAP_FLAG_NONE, &objDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		IID_PPV_ARGS(&m_objectDataBuffer)));

	void* mapped = nullptr;
	THROW_IF_FAILED(m_objectDataBuffer->Map(0, nullptr, &mapped));
	std::memcpy(mapped, gpuObjects.data(), objBytes);
	m_objectDataBuffer->Unmap(0, nullptr);

	// SRV 进 bindless 堆
	{
		GPUDescriptorHandle handle = m_gpuAllocator.Allocate();
		m_objectDataBindlessIndex = handle.index;

		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_UNKNOWN;
		srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Buffer.FirstElement = 0;
		srv.Buffer.NumElements = static_cast<UINT>(gpuObjects.size());
		srv.Buffer.StructureByteStride = sizeof(GPUObjectData);
		device->CreateShaderResourceView(m_objectDataBuffer.Get(), &srv, handle.cpuHandle);
	}
}

void App::TransitionDepth(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES target) {
	if (m_depthState == target) return;

	D3D12_RESOURCE_BARRIER barrier = d3d12lite::CreateTransitionBarrier(
		m_depthBuffer.GetResource(), m_depthState, target);
	cmdList->ResourceBarrier(1, &barrier);
	m_depthState = target;
}