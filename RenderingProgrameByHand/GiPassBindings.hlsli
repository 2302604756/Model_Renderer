#ifndef GI_PASS_BINDINGS_HLSLI
#define GI_PASS_BINDINGS_HLSLI

#include "ShadersCommon.hlsl"

// 与 GiPipeline.cpp 的 GiPassParams 一一对应（b5 根常量，8 DWORD）。
// 这里只放共享声明；ScreenSpaceGI 的函数和 PS 入口保留在 ShadersScreenSpaceGI.hlsl。
cbuffer GiPassCB : register(b5)
{
    uint g_DirectLightingIndex;
    uint g_DiffuseGiIndex;
    uint g_SpecularGiIndex;
    uint g_GiDisplayMode;
    uint g_GiFlags;
    uint g_GiPadding0;
    uint g_GiPadding1;
    uint g_GiPadding2;
};

static const uint GiHasDiffuse = 1u << 0;
static const uint GiHasSpecular = 1u << 1;
static const uint GiEnableDither = 1u << 2;

static const uint GiDisplayFull = 0u;
static const uint GiDisplayDiffuse = 1u;
static const uint GiDisplaySpecular = 2u;

#endif // GI_PASS_BINDINGS_HLSLI
