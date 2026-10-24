#include "RootSignature.h"

RootSignatureBuilder& RootSignatureBuilder::Add32BitConstants(UINT num32BitValues, UINT shaderRegister, UINT registerSpace) {
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    param.Constants.Num32BitValues = num32BitValues;
    param.Constants.ShaderRegister = shaderRegister;
    param.Constants.RegisterSpace = registerSpace;
    m_params.push_back(param);
    return *this;
}

RootSignatureBuilder& RootSignatureBuilder::AddRootCBV(UINT shaderRegister, UINT registerSpace) {
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    param.Descriptor.ShaderRegister = shaderRegister;
    param.Descriptor.RegisterSpace = registerSpace;
    m_params.push_back(param);
    return *this;
}

RootSignatureBuilder& RootSignatureBuilder::AddDefaultStaticSamplers() {
    D3D12_STATIC_SAMPLER_DESC linearWrap{};
    linearWrap.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linearWrap.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    linearWrap.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    linearWrap.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    linearWrap.ShaderRegister = 0; // s0
    linearWrap.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    m_samplers.push_back(linearWrap);

    D3D12_STATIC_SAMPLER_DESC linearClamp = linearWrap;
    linearClamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClamp.ShaderRegister = 1; // s1
    m_samplers.push_back(linearClamp);

    return *this;
}

ComPtr<ID3D12RootSignature> RootSignatureBuilder::Build(ID3D12Device* device, bool allowInputLayout ) {
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(m_params.size());
    desc.pParameters = m_params.data();
    desc.NumStaticSamplers = static_cast<UINT>(m_samplers.size());
    desc.pStaticSamplers = m_samplers.data();

    // 默认开启 SM 6.6 CBV_SRV_UAV 堆直接索引与采样器堆索引
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    if (allowInputLayout) {
        desc.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    }

    ComPtr<ID3DBlob> sigBlob;
    ComPtr<ID3DBlob> errBlob;
    THROW_IF_FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &sigBlob, &errBlob));

    ComPtr<ID3D12RootSignature> rootSig;
    THROW_IF_FAILED(device->CreateRootSignature(
        0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig)
    ));
    return rootSig;
}

RootSignatureBuilder& RootSignatureBuilder::AddRootDescriptorTable(
    UINT shaderRegister, UINT numDescriptors, UINT registerSpace)
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = numDescriptors;
    range.BaseShaderRegister = shaderRegister;
    range.RegisterSpace = registerSpace;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // 每个 table 单独存一组 range，vector 扩容不会让已存 range 的地址失效
    m_ranges.emplace_back(1, range);

    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = m_ranges.back().data();

    m_params.push_back(param);
    return *this;
}