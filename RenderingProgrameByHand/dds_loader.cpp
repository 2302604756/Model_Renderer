#include "dds_loader.h"

#include <fstream>
#include <cstring>

namespace dds {
    namespace {

        constexpr uint32_t DDS_MAGIC = 0x20534444; // "DDS "

        // DDS_HEADER 标志位
        constexpr uint32_t DDSD_MIPMAPCOUNT = 0x20000;

        // DDS_PIXELFORMAT 标志位
        constexpr uint32_t DDPF_FOURCC = 0x4;
        constexpr uint32_t DDPF_RGB = 0x40;
        constexpr uint32_t DDPF_LUMINANCE = 0x20000;

        // DDS_CAPS2
        constexpr uint32_t DDSCAPS2_CUBEMAP = 0x200;

        // D3D10 资源维度
        constexpr uint32_t D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3;
        constexpr uint32_t D3D10_RESOURCE_DIMENSION_TEXTURE3D = 4;

        // D3D10 杂项标志
        constexpr uint32_t DDS_RESOURCE_MISC_TEXTURECUBE = 0x4;

        // FourCC 编码（小端序整数）
        constexpr uint32_t FOURCC_DXT1 = 0x31545844; // "DXT1"
        constexpr uint32_t FOURCC_DXT2 = 0x32545844;
        constexpr uint32_t FOURCC_DXT3 = 0x33545844;
        constexpr uint32_t FOURCC_DXT4 = 0x34545844;
        constexpr uint32_t FOURCC_DXT5 = 0x35545844;
        constexpr uint32_t FOURCC_ATI1 = 0x31495441; // "ATI1"
        constexpr uint32_t FOURCC_ATI2 = 0x32495441; // "ATI2"
        constexpr uint32_t FOURCC_BC4U = 0x55344342; // "BC4U"
        constexpr uint32_t FOURCC_BC4S = 0x53344342;
        constexpr uint32_t FOURCC_BC5U = 0x55354342;
        constexpr uint32_t FOURCC_BC5S = 0x53354342;
        constexpr uint32_t FOURCC_DX10 = 0x30315844; // "DX10"

#pragma pack(push, 1)
        struct DDS_PIXELFORMAT {
            uint32_t size;
            uint32_t flags;
            uint32_t fourCC;
            uint32_t rgbBitCount;
            uint32_t rBitMask;
            uint32_t gBitMask;
            uint32_t bBitMask;
            uint32_t aBitMask;
        };

        struct DDS_HEADER {
            uint32_t       size;
            uint32_t       flags;
            uint32_t       height;
            uint32_t       width;
            uint32_t       pitchOrLinearSize;
            uint32_t       depth;
            uint32_t       mipMapCount;
            uint32_t       reserved1[11];
            DDS_PIXELFORMAT ddspf;
            uint32_t       caps;
            uint32_t       caps2;
            uint32_t       caps3;
            uint32_t       caps4;
            uint32_t       reserved2;
        };

        struct DDS_HEADER_DXT10 {
            uint32_t dxgiFormat;
            uint32_t resourceDimension;
            uint32_t miscFlag;
            uint32_t arraySize;
            uint32_t miscFlags2;
        };
#pragma pack(pop)

        // -------- 格式工具 --------

        // 返回 BC 块的字节数（8 或 16），非 BC 格式返回 0
        uint32_t GetBCBlockSize(DXGI_FORMAT fmt) {
            switch (fmt) {
            case DXGI_FORMAT_BC1_UNORM:
            case DXGI_FORMAT_BC1_UNORM_SRGB:
            case DXGI_FORMAT_BC4_UNORM:
            case DXGI_FORMAT_BC4_SNORM:
                return 8;
            case DXGI_FORMAT_BC2_UNORM:
            case DXGI_FORMAT_BC2_UNORM_SRGB:
            case DXGI_FORMAT_BC3_UNORM:
            case DXGI_FORMAT_BC3_UNORM_SRGB:
            case DXGI_FORMAT_BC5_UNORM:
            case DXGI_FORMAT_BC5_SNORM:
            case DXGI_FORMAT_BC6H_UF16:
            case DXGI_FORMAT_BC6H_SF16:
            case DXGI_FORMAT_BC7_UNORM:
            case DXGI_FORMAT_BC7_UNORM_SRGB:
                return 16;
            default:
                return 0;
            }
        }

        // 未压缩格式每像素字节数；不支持返回 0
        uint32_t GetBitsPerPixel(DXGI_FORMAT fmt) {
            switch (fmt) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_R10G10B10A2_UNORM:
            case DXGI_FORMAT_R16G16_FLOAT:
            case DXGI_FORMAT_R16G16_UNORM:
            case DXGI_FORMAT_R32_FLOAT:
            case DXGI_FORMAT_D32_FLOAT:
            case DXGI_FORMAT_R32_UINT:
                return 32;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_UNORM:
                return 64;
            case DXGI_FORMAT_R8_UNORM:
            case DXGI_FORMAT_A8_UNORM:
                return 8;
            case DXGI_FORMAT_R8G8_UNORM:
                return 16;
            default:
                return 0;
            }
        }

        // 计算某 Mip 的行间距
        uint32_t ComputeRowPitch(DXGI_FORMAT fmt, uint32_t width) {
            if (uint32_t bc = GetBCBlockSize(fmt); bc > 0) {
                uint32_t blocksWide = (width + 3) / 4;
                return blocksWide * bc;
            }
            if (uint32_t bpp = GetBitsPerPixel(fmt); bpp > 0) {
                return (width * bpp + 7) / 8;
            }
            throw std::runtime_error("DDS: unsupported DXGI format for pitch calculation");
        }

        // 传统 DDS 的 FourCC -> DXGI_FORMAT
        DXGI_FORMAT FourCCToFormat(uint32_t fourCC) {
            switch (fourCC) {
            case FOURCC_DXT1: return DXGI_FORMAT_BC1_UNORM;
            case FOURCC_DXT3: return DXGI_FORMAT_BC2_UNORM;
            case FOURCC_DXT5: return DXGI_FORMAT_BC3_UNORM;
            case FOURCC_ATI1:
            case FOURCC_BC4U: return DXGI_FORMAT_BC4_UNORM;
            case FOURCC_BC4S: return DXGI_FORMAT_BC4_SNORM;
            case FOURCC_ATI2:
            case FOURCC_BC5U: return DXGI_FORMAT_BC5_UNORM;
            case FOURCC_BC5S: return DXGI_FORMAT_BC5_SNORM;
            default:
                throw std::runtime_error("DDS: unknown FourCC");
            }
        }

        // 读取未压缩格式的 RGBA 位掩码（只支持常见 32 位格式）
        DXGI_FORMAT UncompressedToFormat(const DDS_PIXELFORMAT& pf) {
            if (pf.rgbBitCount == 32) {
                if (pf.rBitMask == 0x000000FF && pf.gBitMask == 0x0000FF00 &&
                    pf.bBitMask == 0x00FF0000 && pf.aBitMask == 0xFF000000) {
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                }
                if (pf.rBitMask == 0x00FF0000 && pf.gBitMask == 0x0000FF00 &&
                    pf.bBitMask == 0x000000FF && pf.aBitMask == 0xFF000000) {
                    return DXGI_FORMAT_B8G8R8A8_UNORM;
                }
            }
            throw std::runtime_error("DDS: unsupported uncompressed pixel format");
        }

        // -------- 核心解析 --------

        ImageData Parse(const uint8_t* data, size_t size) {
            if (size < 4 + sizeof(DDS_HEADER)) {
                throw std::runtime_error("DDS: file too small");
            }

            const uint8_t* cursor = data;

            uint32_t magic = 0;
            std::memcpy(&magic, cursor, 4);
            cursor += 4;
            if (magic != DDS_MAGIC) {
                throw std::runtime_error("DDS: invalid magic");
            }

            DDS_HEADER header{};
            std::memcpy(&header, cursor, sizeof(DDS_HEADER));
            cursor += sizeof(DDS_HEADER);
            if (header.size != 124) {
                throw std::runtime_error("DDS: invalid header size");
            }

            // 初始化 ImageData
            ImageData img;
            img.width = header.width;
            img.height = header.height;
            img.depth = (header.flags & 0x800000) ? header.depth : 1; // DDSD_DEPTH
            img.mipCount = (header.flags & DDSD_MIPMAPCOUNT) ? header.mipMapCount : 1;
            if (img.mipCount == 0) img.mipCount = 1;
            img.arraySize = 1;

            // 检查是否使用 DX10 扩展头
            bool hasDx10Header = (header.ddspf.flags & DDPF_FOURCC) &&
                (header.ddspf.fourCC == FOURCC_DX10);

            if (hasDx10Header) {
                if (cursor + sizeof(DDS_HEADER_DXT10) > data + size) {
                    throw std::runtime_error("DDS: truncated DX10 header");
                }
                DDS_HEADER_DXT10 h10{};
                std::memcpy(&h10, cursor, sizeof(h10));
                cursor += sizeof(h10);

                img.format = static_cast<DXGI_FORMAT>(h10.dxgiFormat);
                img.arraySize = (h10.arraySize == 0) ? 1 : h10.arraySize;
                img.isCubemap = (h10.miscFlag & DDS_RESOURCE_MISC_TEXTURECUBE) != 0;

                if (h10.resourceDimension == D3D10_RESOURCE_DIMENSION_TEXTURE3D) {
                    img.depth = h10.arraySize; // 3D 纹理的 arraySize 就是 depth
                    img.arraySize = 1;
                }
            }
            else {
                // 传统 DDS
                if (header.ddspf.flags & DDPF_FOURCC) {
                    img.format = FourCCToFormat(header.ddspf.fourCC);
                }
                else if (header.ddspf.flags & DDPF_RGB) {
                    img.format = UncompressedToFormat(header.ddspf);
                }
                else {
                    throw std::runtime_error("DDS: unsupported pixel format");
                }

                // 传统立方体贴图检测
                if (header.caps2 & DDSCAPS2_CUBEMAP) {
                    img.isCubemap = true;
                    img.arraySize = 6;
                }
            }

            // 校验格式是否被支持
            uint32_t bcBlockSize = GetBCBlockSize(img.format);
            uint32_t bpp = GetBitsPerPixel(img.format);
            if (bcBlockSize == 0 && bpp == 0) {
                throw std::runtime_error("DDS: unsupported format for mip walking");
            }

            // 计算所有子资源的总大小并建立 SubresourceData 列表
            // DDS 布局：对每个数组切片，依次存放所有 Mip
            uint64_t totalSize = 0;
            std::vector<SubresourceData> subs;
            subs.reserve(img.mipCount * img.arraySize);

            for (uint32_t slice = 0; slice < img.arraySize; ++slice) {
                uint32_t mipW = img.width;
                uint32_t mipH = img.height;
                uint32_t mipD = img.depth;

                for (uint32_t mip = 0; mip < img.mipCount; ++mip) {
                    SubresourceData sd{};
                    sd.width = mipW;
                    sd.height = mipH;
                    sd.depth = mipD;
                    sd.rowPitch = ComputeRowPitch(img.format, mipW);

                    if (bcBlockSize > 0) {
                        uint32_t blocksHigh = (mipH + 3) / 4;
                        sd.slicePitch = sd.rowPitch * blocksHigh;
                        if (mipD > 1) {
                            sd.slicePitch *= mipD; // 3D 压缩纹理
                        }
                    }
                    else {
                        sd.slicePitch = sd.rowPitch * mipH;
                        if (mipD > 1) {
                            sd.slicePitch *= mipD;
                        }
                    }
                    sd.data = nullptr; // 稍后填
                    subs.push_back(sd);

                    totalSize += sd.slicePitch;

                    // 下一个 Mip 尺寸减半
                    mipW = (mipW > 1) ? mipW / 2 : 1;
                    mipH = (mipH > 1) ? mipH / 2 : 1;
                    mipD = (mipD > 1) ? mipD / 2 : 1;
                }
            }

            // 检查文件长度
            size_t headerSize = static_cast<size_t>(cursor - data);
            if (headerSize + totalSize > size) {
                throw std::runtime_error("DDS: file data truncated");
            }

            // 拷贝原始数据
            img.rawData.resize(totalSize);
            std::memcpy(img.rawData.data(), cursor, totalSize);

            // 修正 SubresourceData::data 指针指向 rawData
            uint64_t offset = 0;
            for (auto& sd : subs) {
                sd.data = img.rawData.data() + offset;
                offset += sd.slicePitch;
            }
            img.subresources = std::move(subs);

            return img;
        }

    } // namespace

    ImageData LoadFromFile(const std::string& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            throw std::runtime_error("DDS: cannot open file: " + path);
        }
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> buffer(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
            throw std::runtime_error("DDS: failed to read file: " + path);
        }

        return LoadFromMemory(buffer.data(), buffer.size());
    }

    ImageData LoadFromMemory(const uint8_t* data, size_t size) {
        return Parse(data, size);
    }

} // namespace dds