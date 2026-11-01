#include "d3d12lite.h"
#include "RenderContext.h" // 仅在 cpp 中包含，避免头文件循环依赖

namespace d3d12lite {
    ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        uint32_t numDescriptors,
        D3D12_DESCRIPTOR_HEAP_FLAGS flags
    ) {
        ID3D12Device* device = RenderContext::Get().GetDevice();
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc;
        heapDesc.NumDescriptors = numDescriptors;
        heapDesc.Type = type;
        heapDesc.Flags = flags;
        heapDesc.NodeMask = 0;
        ComPtr<ID3D12DescriptorHeap> heap;
        THROW_IF_FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)));
        return heap;
    }



    ////////////////////////////////////////////////////////////////////////////////
    /////////
    /////////Resource部分
    /////////
    ////////////////////////////////////////////////////////////////////////////////
    ComPtr<ID3D12Heap> d3d12lite::CreateHeap(D3D12_HEAP_TYPE type, uint64_t size) {
        ID3D12Device* device = RenderContext::Get().GetDevice();

        ComPtr<ID3D12Heap> heap;
        D3D12_HEAP_PROPERTIES properties = {};
        properties.Type = type;
        D3D12_HEAP_DESC desc = {};
        desc.SizeInBytes = size;
        desc.Flags = D3D12_HEAP_FLAG_NONE;
        desc.Properties = properties;
        desc.Alignment = 0;

        device->CreateHeap(&desc, IID_PPV_ARGS(&heap));
        return heap;
    }

    ComPtr<ID3D12Resource> d3d12lite::CreateCommittedDefualtResource(dds::ImageData image) {
        ID3D12Device* device = RenderContext::Get().GetDevice();
        D3D12_HEAP_PROPERTIES heapProp;
        ComPtr<ID3D12Resource> texture;

        //  填写 D3D12_RESOURCE_DESC
        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension = (image.depth > 1)
            ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
            : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = image.width;
        texDesc.Height = image.height;
        texDesc.DepthOrArraySize = (image.depth > 1)
            ? static_cast<UINT16>(image.depth)
            : static_cast<UINT16>(image.arraySize);
        texDesc.MipLevels = static_cast<UINT16>(image.mipCount);
        texDesc.Format = image.format;
        texDesc.SampleDesc = { 1, 0 };
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        heapProp.Type = D3D12_HEAP_TYPE_DEFAULT;
        device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
        return texture;
    }

    ComPtr<ID3D12Resource> d3d12lite::CreateTexture2D(std::string path) {
        ID3D12Device* device = RenderContext::Get().GetDevice();
        // 1. 加载 DDS
        dds::ImageData image = dds::LoadFromFile(path);

        //2.填写desc
        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension = (image.depth > 1)
            ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
            : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = image.width;
        texDesc.Height = image.height;
        texDesc.DepthOrArraySize = (image.depth > 1)
            ? static_cast<UINT16>(image.depth)
            : static_cast<UINT16>(image.arraySize);
        texDesc.MipLevels = static_cast<UINT16>(image.mipCount);
        texDesc.Format = image.format;
        texDesc.SampleDesc = { 1, 0 };
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;


        // 3. 创建默认堆纹理
        ComPtr<ID3D12Resource> texture;
        D3D12_HEAP_PROPERTIES defaultHeap = {};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;


        device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&texture));

        //3.算上传布局
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(image.subresources.size());
        uint64_t uploadSize = 0;
        device->GetCopyableFootprints(
            &texDesc, 0, static_cast<UINT>(layouts.size()), 0,
            layouts.data(), nullptr, nullptr, &uploadSize);

        //5.创建上传堆并逐行memcpy
        ComPtr<ID3D12Resource> uploadBuffer;
        D3D12_HEAP_PROPERTIES uploadHeap = {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; // 必须是 Buffer
        uploadDesc.Alignment = 0;
        uploadDesc.Width = uploadSize;          // <--- 直接写进 Width
        uploadDesc.Height = 1;                               // 缓冲区高必须为 1
        uploadDesc.DepthOrArraySize = 1;                               // 深度/切片必须为 1
        uploadDesc.MipLevels = 1;                               // Mip 层级必须为 1
        uploadDesc.Format = DXGI_FORMAT_UNKNOWN;             // Buffer 统一填 UNKNOWN
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.SampleDesc.Quality = 0;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;  // Buffer 必须是行主序
        uploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&uploadBuffer));

        uint8_t* mapped = nullptr;
        uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));

        for (size_t i = 0; i < image.subresources.size(); ++i) {
            const auto& src = image.subresources[i];
            const auto& dst = layouts[i];

            uint8_t* dstSlice = mapped + dst.Offset;
            const uint8_t* srcSlice = src.data;

            // BC 格式按"块行"拷贝，非 BC 格式按"像素行"拷贝
            // 因为 src.rowPitch 是紧密排布，dst.Footprint.RowPitch 是 256 字节对齐的
            uint32_t rowCount = (image.format >= DXGI_FORMAT_BC1_TYPELESS &&
                image.format <= DXGI_FORMAT_BC7_UNORM_SRGB)
                ? (src.height + 3) / 4   // BC：块行数
                : src.height;            // 非 BC：像素行数

            for (uint32_t y = 0; y < rowCount; ++y) {
                std::memcpy(
                    dstSlice + y * dst.Footprint.RowPitch,
                    srcSlice + y * src.rowPitch,
                    src.rowPitch  // 实际数据量
                );
            }


        }

        uploadBuffer->Unmap(0, nullptr);

        //获取帧资源
        FrameResource& frame = RenderContext::Get().GetFrame();
        for (size_t i = 0; i < image.subresources.size(); i++) {

            //目标默认堆的配置
            D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
            dstLocation.pResource = texture.Get();
            dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLocation.SubresourceIndex = static_cast<UINT>(i);

            //源头上传堆的配置
            D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
            srcLocation.pResource = uploadBuffer.Get();
            srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLocation.PlacedFootprint = layouts[i];

            //执行拷贝
            frame.cmdList->CopyTextureRegion(
                &dstLocation,   // 目标：默认堆纹理
                0, 0, 0,        // 目标偏移 (x, y, z)
                &srcLocation,   // 源：上传堆的 placed footprint
                nullptr         // 源矩形 nullptr 表示拷贝整个子资源
            );

        }

        return texture;
    }



  

    //资源屏障
    // 构造：资源状态转换屏障
    D3D12_RESOURCE_BARRIER CreateTransitionBarrier(
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES stateBefore,
        D3D12_RESOURCE_STATES stateAfter,
        UINT subresource,
        D3D12_RESOURCE_BARRIER_FLAGS flags
    ) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = flags;
        barrier.Transition.Subresource = subresource;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = stateBefore;
        barrier.Transition.StateAfter = stateAfter;
        return barrier;
    }
    // 构造：UAV 屏障 (用于 UAV 读写同步)
    D3D12_RESOURCE_BARRIER CreateUAVBarrier(
        ID3D12Resource* resource
    ) {
        D3D12_RESOURCE_BARRIER barrier;
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.UAV.pResource = resource;
        return barrier;
    }

    // 构造：别名屏障 (用于堆内存复用)
    D3D12_RESOURCE_BARRIER CreateAliasingBarrier(
        ID3D12Resource* resourceBefore,
        ID3D12Resource* resourceAfter
    ) {
        D3D12_RESOURCE_BARRIER barrier;
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Aliasing.pResourceBefore = resourceBefore;
        barrier.Aliasing.pResourceAfter = resourceAfter;
        return barrier;
    }
     
     //临时的Vertex buffer

    MeshBuffer CreateTexturedTriangle(ID3D12Device* device) {
        const Vertex vertices[] = {
            { {  0.0f,  0.5f, 0.0f }, { 0.5f, 0.0f } }, // 顶点：顶部中间
            { {  0.5f, -0.5f, 0.0f }, { 1.0f, 1.0f } }, // 顶点：右下
            { { -0.5f, -0.5f, 0.0f }, { 0.0f, 1.0f } }, // 顶点：左下
        };
        const uint32_t bufferSize = sizeof(vertices);

        MeshBuffer mesh = {};
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bufferSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        THROW_IF_FAILED(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&mesh.resource)
        ));

        uint8_t* pData = nullptr;
        THROW_IF_FAILED(mesh.resource->Map(0, nullptr, reinterpret_cast<void**>(&pData)));
        memcpy(pData, vertices, bufferSize);
        mesh.view.BufferLocation = mesh.resource->GetGPUVirtualAddress();
        mesh.view.StrideInBytes = sizeof(Vertex);
        mesh.view.SizeInBytes = bufferSize;
        return mesh;
    }


    // 运行时将 .hlsl 编译为 SM 6.6 字节码 (例如 target 传 L"vs_6_6" 或 L"ps_6_6")
    std::vector<uint8_t> d3d12lite::CompileShaderSM66(
        const std::wstring& filePath,
        const std::wstring& entryPoint,
        const std::wstring& targetProfile)
    {
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        THROW_IF_FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)));
        THROW_IF_FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));

        // 1. 加载着色器源文件
        uint32_t codePage = DXC_CP_UTF8;
        ComPtr<IDxcBlobEncoding> sourceBlob;
        THROW_IF_FAILED(utils->LoadFile(filePath.c_str(), &codePage, &sourceBlob));

        DxcBuffer sourceBuffer{};
        sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
        sourceBuffer.Size = sourceBlob->GetBufferSize();
        sourceBuffer.Encoding = DXC_CP_UTF8;

        // 2. 组装编译参数 (-E 入口点 -T 目标版本 -Zi 调试信息)
        std::vector<LPCWSTR> arguments = {
            filePath.c_str(),
            L"-E", entryPoint.c_str(),
            L"-T", targetProfile.c_str(),
#if defined(_DEBUG)
            L"-Zi", L"-Qembed_debug", L"-Od" // Debug 模式下禁用优化并嵌入调试信息，方便 RenderDoc 源码级调试
#else
            L"-O3"
#endif
        };

        // 3. 创建默认 include handler
        ComPtr<IDxcIncludeHandler> includeHandler;
        THROW_IF_FAILED(utils->CreateDefaultIncludeHandler(&includeHandler));

        // 4. 执行编译（把 nullptr 换成 includeHandler）
        ComPtr<IDxcResult> result;
        THROW_IF_FAILED(compiler->Compile(
            &sourceBuffer,
            arguments.data(),
            static_cast<uint32_t>(arguments.size()),
            includeHandler.Get(),          // ← 改这里
            IID_PPV_ARGS(&result)));

        // 5. 检查编译错误日志
        ComPtr<IDxcBlobUtf8> errors;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        if (errors && errors->GetStringLength() > 0) {
            OutputDebugStringA(errors->GetStringPointer());
        }

        HRESULT status;
        result->GetStatus(&status);
        if (FAILED(status)) {
            throw std::runtime_error("DXC Shader Compilation Failed: " +
                std::string(errors ? errors->GetStringPointer() : "Unknown error"));
        }

        // 5. 提取编译后的 DXIL 二进制字节码
        ComPtr<IDxcBlob> shaderObject;
        THROW_IF_FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shaderObject), nullptr));

        const uint8_t* ptr = static_cast<const uint8_t*>(shaderObject->GetBufferPointer());
        return std::vector<uint8_t>(ptr, ptr + shaderObject->GetBufferSize());
    }
}