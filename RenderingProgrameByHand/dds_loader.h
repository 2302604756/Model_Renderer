#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <stdexcept>
#include <dxgiformat.h> // 只依赖 DXGI_FORMAT 枚举，不依赖 d3d12.h

namespace dds {

    // 单个子资源（一个 Mip 层 / 一个数组切片 / 一个立方体面）
    struct SubresourceData {
        const uint8_t* data = nullptr;  // 指向 rawData 内部的指针
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 0;
        uint32_t rowPitch = 0;        // 每行字节数（BC 格式为每行块的字节数）
        uint32_t slicePitch = 0;        // 整个 2D 切片大小（3D 纹理用）
    };

    struct ImageData {
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t    width = 0;
        uint32_t    height = 0;
        uint32_t    depth = 0;       // 3D 纹理深度，否则为 1
        uint32_t    mipCount = 1;
        uint32_t    arraySize = 1;
        bool        isCubemap = false;

        // 所有子资源，按 D3D12 子资源顺序排列：
        //   subresource = mipSlice + arraySlice * mipCount
        std::vector<SubresourceData> subresources;

        // 原始数据（拥有所有权，SubresourceData::data 指向它）
        std::vector<uint8_t> rawData;
    };

    // 从磁盘加载
    ImageData LoadFromFile(const std::string& path);

    // 从内存加载
    ImageData LoadFromMemory(const uint8_t* data, size_t size);

} // namespace dds