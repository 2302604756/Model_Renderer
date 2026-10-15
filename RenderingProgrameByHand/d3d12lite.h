#pragma once

#include <d3d12.h>
#include <wrl.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <dxgi1_6.h>

#include <dxcapi.h> // Windows SDK 自带 DXC 头文件
#include "dds_loader.h"

#pragma comment(lib, "dxcompiler.lib") // 链接 DXC 库

using Microsoft::WRL::ComPtr;

#define THROW_IF_FAILED(hr)                                                     \
    do {                                                                        \
        HRESULT _hr = (hr);                                                     \
        if (FAILED(_hr)) {                                                      \
            throw std::runtime_error(std::string("D3D12 Error in ") + __FILE__  \
                + " at line " + std::to_string(__LINE__)                        \
                + " Code: " + std::to_string(_hr));                             \
        }                                                                       \
    } while (0)

namespace d3d12lite {
    // 现代 C++ 风格：直接返回智能指针，调用端一行赋值
    ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        uint32_t numDescriptors,
        D3D12_DESCRIPTOR_HEAP_FLAGS flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE
    );

    // 便捷包装：创建 RTV 专用堆
    inline ComPtr<ID3D12DescriptorHeap> CreateRTVHeap(uint32_t numDescriptors) {
        return CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, numDescriptors, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    }
    //便捷包装：创建 DSV 专用堆
    inline ComPtr<ID3D12DescriptorHeap> CreateDSVHeap(uint32_t numDescriptors) {
        return CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, numDescriptors, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    }

    inline ComPtr<ID3D12DescriptorHeap> CreateSRVHeap(uint32_t numDescriptors) {
        return CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, numDescriptors, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    }


    ////////////////////////////////////////////////////////////////////////////////
    /////////
    /////////Resource部分
    /////////
    ////////////////////////////////////////////////////////////////////////////////
    
    //Heap
    inline ComPtr<ID3D12Heap> CreateHeap(D3D12_HEAP_TYPE type, uint64_t size);
    inline ComPtr<ID3D12Heap> CreateDefaultHeap(uint64_t size) { return CreateHeap(D3D12_HEAP_TYPE_DEFAULT, size); }
    inline ComPtr<ID3D12Heap> CreateGPUUploadHeap(uint64_t size) { return CreateHeap(D3D12_HEAP_TYPE_GPU_UPLOAD, size); }
    inline ComPtr<ID3D12Heap> CreateUploadHeap(uint64_t size) { return CreateHeap(D3D12_HEAP_TYPE_UPLOAD, size); }
    //Resource
    inline ComPtr<ID3D12Resource> CreateCommittedDefualtResource(dds::ImageData image);
    inline ComPtr<ID3D12Resource> CreateTexture2D(std::string path);
    

    
    //临时渲染
   
    //资源屏障
    // 构造：资源状态转换屏障 (最常用)
    D3D12_RESOURCE_BARRIER CreateTransitionBarrier(
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES stateBefore,
        D3D12_RESOURCE_STATES stateAfter,
        UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_BARRIER_FLAGS flags = D3D12_RESOURCE_BARRIER_FLAG_NONE
    );

    // 构造：UAV 屏障 (用于 UAV 读写同步)
    D3D12_RESOURCE_BARRIER CreateUAVBarrier(
        ID3D12Resource* resource = nullptr
    );

    // 构造：别名屏障 (用于堆内存复用)
    D3D12_RESOURCE_BARRIER CreateAliasingBarrier(
        ID3D12Resource* resourceBefore,
        ID3D12Resource* resourceAfter
    );

    //临时的signature创建方法
    inline ComPtr<ID3D12RootSignature> CreateBindlessRootSignature(ID3D12Device* device) {
        // 1. Root Constants 参数 (传递 textureIndex)
        D3D12_ROOT_PARAMETER rootParam{};
        rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParam.Constants.ShaderRegister = 0; // b0
        rootParam.Constants.RegisterSpace = 0;
        rootParam.Constants.Num32BitValues = 4; // 16 字节

        // 2. 静态线性采样器 (s0)
        D3D12_STATIC_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.ShaderRegister = 0; // s0
        samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // 3. 根签名标志
        D3D12_ROOT_SIGNATURE_DESC rootSigDesc{};
        rootSigDesc.NumParameters = 1;
        rootSigDesc.pParameters = &rootParam;
        rootSigDesc.NumStaticSamplers = 1;
        rootSigDesc.pStaticSamplers = &samplerDesc;
        rootSigDesc.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED; // 关键：开启 SM 6.6 堆直接寻址

        ComPtr<ID3DBlob> signatureBlob;
        ComPtr<ID3DBlob> errorBlob;
        THROW_IF_FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &signatureBlob, &errorBlob));

        ComPtr<ID3D12RootSignature> rootSig;
        THROW_IF_FAILED(device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig)));
        return rootSig;
    }

    //临时的Vertex Buffer
    // 在 d3d12lite.h 的 namespace d3d12lite 内新增：
    struct Vertex {
        float pos[3];
        float uv[2];
    };

    struct MeshBuffer {
        ComPtr<ID3D12Resource>   resource;
        D3D12_VERTEX_BUFFER_VIEW view{};
    };

    MeshBuffer CreateTexturedTriangle(ID3D12Device* device);

    // 运行时将 .hlsl 编译为 SM 6.6 字节码 (例如 target 传 L"vs_6_6" 或 L"ps_6_6")
    std::vector<uint8_t> CompileShaderSM66(
        const std::wstring& filePath,
        const std::wstring& entryPoint,
        const std::wstring& targetProfile);
   
}