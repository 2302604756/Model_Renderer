#pragma once
#include "d3d12lite.h"
#include "RenderContext.h"
#include "CPUDescriptorAllocator.h"
#include "BindlessDescriptorHeap.h"
#include "TransientHeap.h"
#include "UploadHeap.h"
#include "PipelineBuilder.h"
#include "Texture2D.h"

//用一层应用层去包裹
//持有 pso，heap,,rendercontext,descriptor heap
//


class App {
public:
	App() = default;
	//禁止拷贝
	App(const App& app) = delete;
	App operator=(const App&) = delete;

	void init(HWND hwnd);
	//每帧调度渲染函数
	void RenderTick();

private:
	RenderContext			m_context;
	CPUDescriptorAllocator  m_cpuAllocator;
	GPUDescriptorAllocator  m_gpuAllocator;
	TransientHeap           m_physicalHeap;
	UploadHeap				m_uploadHeap;
	PipelineBuilder			m_pipelineBuilder;

	// 渲染管线与几何资产
	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pso;
	d3d12lite::MeshBuffer       m_triangleMesh;
	Texture2D                   m_texture;
};
