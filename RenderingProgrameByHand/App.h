#pragma once
#include "d3d12lite.h"
#include "RenderContext.h"
#include "CPUDescriptorAllocator.h"
#include "BindlessDescriptorHeap.h"
#include "TransientHeap.h"
#include "UploadHeap.h"
#include "PipelineBuilder.h"
#include "Texture2D.h"
#include "RootSignature.h"
#include "DepthBuffer.h"

#include "Mesh.h"
#include "Model.h"
#include "MeshLibrary.h"
#include "RenderTexture.h"
#include "SceneTypes.h"
#include "AccelerationStructure.h"

//Pass
#include "RenderGraph.h"
#include "GBufferPass.h"
#include "LightingPass.h"
#include "ComputeSmokePass.h"
#include "ProbeGrid.h"
#include "ProbeUpdatePass.h"
#include "ProbeBorderFixupPass.h"
#include "ProbeDistancePass.h"


//CPU端与GPU端场景数据构建
#include "Scene.h"
#include "SceneGpuData.h"
#include "LightingTypes.h"  

#include "Camera.h"
#include "Input.h"

#include <DirectXMath.h>
#include <chrono>						//用于高精度计时 测量FPS
#include <vector>


//用一层应用层去包裹
//持有 pso，heap,,rendercontext,descriptor heap
//



class App {
public:
	App() = default;
	//禁止拷贝
	App(const App& app) = delete;
	App operator=(const App& app) = delete;

	void init(HWND hwnd);
	//每帧调度渲染函数
	void RenderTick();

	// WindowProc 转发输入
	void OnKeyDown(WPARAM key) { m_input.OnKeyDown(key); }
	void OnKeyUp(WPARAM key) { m_input.OnKeyUp(key); }
	void OnFocusLost() { m_input.OnFocusLost(); }
	void OnMouseMove(int x, int y) { m_input.OnMouseMove(x, y); }
	void OnMouseButtonDown(int x, int y) { m_input.OnMouseButtonDown(x, y); }
	void OnMouseButtonUp() { m_input.OnMouseButtonUp(); }


private:
	//与Camera的联系
	CameraControl BuildCameraControl() const;


	// 渲染管线与几何资产
	ComPtr<ID3D12RootSignature> m_rootSignature;
	MeshLibrary					m_meshLibrary;		
	Texture2D                   m_texture;			
	DepthBuffer					m_depthBuffer;

	//场景ID：该Mesh在m_meshLibrary中的vector<Mesh> 中的索引
	uint32_t m_cubeMeshId = 0;
	uint32_t m_sphereMeshId = 0;
	uint32_t m_planeMeshId = 0;
	uint32_t m_textureIndex = 0;
	//帧ID
	uint32_t m_frameCounter = 0;

	//光追
	rt::AccelerationScene       m_rtScene;

	//CPU与GPU场景数据
	Scene        m_scene;
	SceneGpuData m_sceneGpuData;

	//光源
	std::vector<GPULight> m_gpuLights;
	LightingConstants     m_lightingConstants{};
	void BuildLights();   // CPU Light → GPULight[] + LightingConstants


	RenderTexture m_gbufferAlbedo;
	RenderTexture m_gbufferNormal;
	uint32_t      m_depthBindlessIndex = UINT32_MAX;


	//Pass
	RenderGraph           m_graph;
	GBufferPass           m_gbufferPass;
	LightingPass          m_lightingPass;

	RenderGraph::ResourceId m_gbufferAlbedoId = RenderGraph::kInvalidId;
	RenderGraph::ResourceId m_gbufferNormalId = RenderGraph::kInvalidId;
	RenderGraph::ResourceId m_depthId = RenderGraph::kInvalidId;
	std::vector<RenderGraph::ResourceId> m_backbufferIds;

	//探针Pass
	ProbeGrid        m_probeGrid;
	ProbeUpdatePass  m_probeUpdatePass;
	ComPtr<ID3D12RootSignature> m_probeRootSignature;
	ComPtr<ID3D12PipelineState> m_probePSO;
	RenderGraph::ResourceId     m_probeGridId = RenderGraph::kInvalidId;
	ProbeBorderFixupPass m_probeBorderFixupPass;
	ComPtr<ID3D12PipelineState> m_probeBorderPSO;
	ProbeDistancePass   m_probeDistancePass;          // 距离矩更新
	ComPtr<ID3D12PipelineState> m_probeDistancePSO;   // 距离矩 PSO
	RenderGraph::ResourceId     m_probeDistanceId = RenderGraph::kInvalidId;
	ProbeBorderFixupPass m_probeDistanceBorderPass;   // 距离矩 gutter

	//用于窗口状态刷新
	HWND                                                 m_hwnd = nullptr;
	std::chrono::high_resolution_clock::time_point       m_lastFrameTime;
	uint32_t                                             m_fpsFrameCount = 0;
	float                                                m_fpsTimeElapsed = 0.0f;
	float												 m_recordCpuMsAccum = 0.0f;
	uint32_t											 m_recordCpuSamples = 0;
	float												 m_waitMsAccum = 0.0f;
	float												 m_tickCpuMsAccum = 0.0f;
	float												 m_beginMsAccum = 0.0f;
	float												 m_executeMsAccum = 0.0f;
	float												 m_presentMsAccum = 0.0f;
	uint32_t											 m_waitHitCount = 0;

	//基础设施
	RenderContext			m_context;
	CPUDescriptorAllocator  m_cpuAllocator;
	GPUDescriptorAllocator  m_gpuAllocator;
	TransientHeap           m_physicalHeap;
	UploadHeap				m_uploadHeap;
	PipelineBuilder			m_pipelineBuilder;

	void UpdateFrameStats(float deltaTime);   

	Camera m_camera;
	Input  m_input;

	//多pass pso
	ComPtr<ID3D12PipelineState> m_gbufferPSO;
	ComPtr<ID3D12PipelineState> m_lightingPSO;
	uint32_t m_debugMode = 0;

	//Compute Shader参数
	ComPtr<ID3D12RootSignature> m_computeRootSignature;
	ComPtr<ID3D12PipelineState> m_computePSO;

	RenderTexture    m_computeTexture;
	ComputeSmokePass m_computeSmokePass;

	RenderGraph::ResourceId m_computeTextureId = RenderGraph::kInvalidId;
};
