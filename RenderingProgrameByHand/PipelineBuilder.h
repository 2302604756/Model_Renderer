#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <fstream>
#include <stdexcept>
#include "RenderContext.h"

using Microsoft::WRL::ComPtr;

inline std::vector<uint8_t> LoadCSOBlob(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Failed to load CSO: " + path);
    size_t size = static_cast<size_t>(file.tellg());
    std::vector<uint8_t> buffer(size);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), size);
    return buffer;
}

class PipelineBuilder {
public:
    PipelineBuilder() {
        //设置一些基础性的必须属性
        m_desc.SampleMask = UINT_MAX;
        m_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        m_desc.SampleDesc.Count = 1;
        m_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        m_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        m_desc.RasterizerState.DepthClipEnable = TRUE;
        m_desc.RasterizerState.FrontCounterClockwise = TRUE;

        m_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        m_desc.DepthStencilState.DepthEnable = TRUE;


    }



    PipelineBuilder& SetRootSignature(ID3D12RootSignature* rs) {
        m_desc.pRootSignature = rs;
        return *this;
    }

    PipelineBuilder& SetInputLayout(const D3D12_INPUT_ELEMENT_DESC* layout, uint32_t count) {
        m_desc.InputLayout = { layout,count };
        return *this;
    }

    PipelineBuilder& SetShaders(const std::vector<uint8_t>& vs, const std::vector<uint8_t>& ps) {
        m_desc.VS = { vs.data(), vs.size() };
        m_desc.PS = { ps.data(), ps.size() };
        return *this;
    }

    PipelineBuilder& SetRTVFormat(DXGI_FORMAT format) {
        m_desc.NumRenderTargets = 1;
        m_desc.RTVFormats[0] = format;
        return *this;
    }

    PipelineBuilder& SetDepthFormat(DXGI_FORMAT format) {
        m_desc.DSVFormat = format;                                  // PSO 必须声明深度格式
        m_desc.DepthStencilState.DepthEnable = TRUE;
        m_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        m_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS; // 越近越先通过
        return *this;
    }


    ComPtr<ID3D12PipelineState> Build(ID3D12Device* device) {
        ComPtr<ID3D12PipelineState> pso;
        THROW_IF_FAILED(device->CreateGraphicsPipelineState(&m_desc, IID_PPV_ARGS(&pso)));
        return pso;
    }

    ComPtr<ID3D12PipelineState> BuildCompute(
        ID3D12Device* device,
        ID3D12RootSignature* rootSignature,
        const std::vector<uint8_t>& cs)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = rootSignature;
        desc.CS = { cs.data(), cs.size() };

        ComPtr<ID3D12PipelineState> pso;
        THROW_IF_FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
        return pso;
    }

    PipelineBuilder& SetRTVFormats(const DXGI_FORMAT* formats, uint32_t count) {
        m_desc.NumRenderTargets = count;
        for (uint32_t i = 0; i < count; ++i) {
            m_desc.RTVFormats[i] = formats[i];
            // MRT 的每个目标都必须显式打开写掩码，否则 RT1 不会被写
            m_desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        return *this;
    }

    PipelineBuilder& SetDepthDisabled() {
        m_desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        m_desc.DepthStencilState.DepthEnable = FALSE;
        m_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        m_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        return *this;
    }

    PipelineBuilder& SetCullNone() {
        m_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        return *this;
    }

private:
    D3D12_GRAPHICS_PIPELINE_STATE_DESC m_desc = {};
};