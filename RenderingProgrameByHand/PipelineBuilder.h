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
        m_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        m_desc.DepthStencilState.DepthEnable = FALSE;
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

    ComPtr<ID3D12PipelineState> Build(ID3D12Device* device) {
        ComPtr<ID3D12PipelineState> pso;
        THROW_IF_FAILED(device->CreateGraphicsPipelineState(&m_desc, IID_PPV_ARGS(&pso)));
        return pso;
    }

private:
    D3D12_GRAPHICS_PIPELINE_STATE_DESC m_desc = {};
};