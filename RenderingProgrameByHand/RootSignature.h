#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include "RenderContext.h"

using Microsoft::WRL::ComPtr;

class RootSignatureBuilder {
public:
	// 添加 32 位 Push Constants (用于传递 Bindless 索引)
	RootSignatureBuilder& Add32BitConstants(UINT num32BitValues, UINT shaderRegister, UINT registerSpace = 0);

	// 添加 Root CBV (直接绑定逐帧瞬态分配器返回的 GPU 虚拟地址)
	RootSignatureBuilder& AddRootCBV(UINT shaderRegister, UINT registerSpace = 0);

	// 添加默认静态采样器 (s0: 线性重复, s1: 线性夹取)
	RootSignatureBuilder& AddDefaultStaticSamplers();

	// 在 RootSignatureBuilder 中新增：
	inline RootSignatureBuilder& AddRootSRV(UINT shaderRegister, UINT registerSpace = 0) {
		D3D12_ROOT_PARAMETER param{};
		param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; // 直接绑定大 Buffer 虚拟地址
		param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		param.Descriptor.ShaderRegister = shaderRegister;
		param.Descriptor.RegisterSpace = registerSpace;
		m_params.push_back(param);
		return *this;
	}

	// 添加一个只含 SRV 的 descriptor table（用于绑定 TLAS 这类必须走描述符的资源）
	RootSignatureBuilder& AddRootDescriptorTable(
		UINT shaderRegister, UINT numDescriptors = 1, UINT registerSpace = 0);

	ComPtr<ID3D12RootSignature> Build(ID3D12Device* device, bool allowInputLayout = true);
private:
	std::vector<D3D12_ROOT_PARAMETER>		 m_params;
	std::vector<D3D12_STATIC_SAMPLER_DESC>   m_samplers;
	std::vector<std::vector<D3D12_DESCRIPTOR_RANGE>>	 m_ranges;
};