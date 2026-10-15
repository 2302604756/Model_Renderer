#include "App.h"

void App::init(HWND hwnd) {
	// 1. 初始化核心上下文与各大分配器
	m_context.Init(hwnd, 1920, 1080);
	m_cpuAllocator.Init();
	m_gpuAllocator.Init();
	m_physicalHeap.Init();
	m_uploadHeap.Init();

	ID3D12Device* device = m_context.GetDevice();

	// 2. 开启当前槽位的 CommandList 以录制初始化拷贝指令
	FrameResource& frame = m_context.GetFrame();
	THROW_IF_FAILED(frame.cmdAllocator->Reset());
	THROW_IF_FAILED(frame.cmdList->Reset(frame.cmdAllocator.Get(), nullptr));

	// 录制纹理上传指令并直写 Bindless 堆
	m_texture = Texture2D::LoadFromFilePlaced(
		R"(D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\assets\test.dds)",
		m_physicalHeap, m_uploadHeap, m_gpuAllocator
	);

	// 提交拷贝指令并等待 GPU 彻底完成搬运
	THROW_IF_FAILED(frame.cmdList->Close());
	ID3D12CommandList* const lists[] = { frame.cmdList.Get() };
	m_context.GetCommandQueue()->ExecuteCommandLists(1, lists); // 需在 RenderContext.h 暴露 GetCommandQueue()
	m_context.WaitForGPU();

	// 拷贝完成后，重置 UploadHeap 游标以释放上传空间
	m_uploadHeap.Reset();

	// 3. 创建三角形顶点缓冲
	m_triangleMesh = d3d12lite::CreateTexturedTriangle(device);

	// 4. 创建 Bindless 根签名！
	m_rootSignature = d3d12lite::CreateBindlessRootSignature(device);

	// 直接对同一个着色器文件分别提取 VSMain 和 PSMain
	auto vsBlob = d3d12lite::CompileShaderSM66(L"Shaders.hlsl", L"VSMain", L"vs_6_6");
	auto psBlob = d3d12lite::CompileShaderSM66(L"Shaders.hlsl", L"PSMain", L"ps_6_6");
	assert(!vsBlob.empty() && !psBlob.empty());

	const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	m_pso = m_pipelineBuilder
		.SetRootSignature(m_rootSignature.Get())
		.SetInputLayout(inputLayout, _countof(inputLayout))
		.SetShaders(vsBlob, psBlob)
		.SetRTVFormat(DXGI_FORMAT_R8G8B8A8_UNORM)
		.Build(device);
}
void App::RenderTick() {
	m_context.BeginFrame();

	FrameResource& frame = m_context.GetFrame();
	ID3D12GraphicsCommandList* cmdList = frame.cmdList.Get();
	uint32_t frameIndex = m_context.GetFrameIndex();

	// 1. 从 CPU 分配器获取当前后缓冲资源与 RTV 句柄
	ID3D12Resource* backBuffer = m_cpuAllocator.GetBackBufferResource(frameIndex);
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_cpuAllocator.GetBackBufferRTV(frameIndex);

	// 2. 状态屏障：PRESENT -> RENDER_TARGET
	D3D12_RESOURCE_BARRIER barrierBefore = d3d12lite::CreateTransitionBarrier(
		backBuffer,
		D3D12_RESOURCE_STATE_PRESENT,
		D3D12_RESOURCE_STATE_RENDER_TARGET
	);
	cmdList->ResourceBarrier(1, &barrierBefore);

	// 3. 设置视口与裁剪矩形
	D3D12_VIEWPORT viewport = { 0.0f, 0.0f, 1920.0f, 1080.0f, 0.0f, 1.0f };
	D3D12_RECT scissorRect = { 0, 0, 1920, 1080 };
	cmdList->RSSetViewports(1, &viewport);
	cmdList->RSSetScissorRects(1, &scissorRect);

	// 4. 清屏并绑定渲染目标
	cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
	const float clearColor[] = { 0.1f, 0.1f, 0.15f, 1.0f };
	cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

	// 5. Bindless 核心绘制指令
	// (a) 绑定全局 Shader-Visible 堆
	ID3D12DescriptorHeap* heaps[] = { m_gpuAllocator.GetHeap() };
	cmdList->SetDescriptorHeaps(1, heaps);

	// (b) 绑定根签名与管线状态
	cmdList->SetGraphicsRootSignature(m_rootSignature.Get());
	cmdList->SetPipelineState(m_pso.Get());

	// (c) 将纹理的 Bindless 槽位索引通过 32 位根常量直接推给 Shader
	cmdList->SetGraphicsRoot32BitConstant(0, m_texture.GetBindlessIndex(), 0);

	// (d) 绑定顶点缓冲并下发 DrawCall
	cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmdList->IASetVertexBuffers(0, 1, &m_triangleMesh.view);
	cmdList->DrawInstanced(3, 1, 0, 0);

	// 6. 状态屏障：RENDER_TARGET -> PRESENT
	D3D12_RESOURCE_BARRIER barrierAfter = d3d12lite::CreateTransitionBarrier(
		backBuffer,
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PRESENT
	);
	cmdList->ResourceBarrier(1, &barrierAfter);

	m_context.EndFrame();
}