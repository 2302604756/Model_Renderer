#pragma once
#include <cstdint>
#include <DirectXMath.h>

// 对应 ShadersProbe.hlsl 的 ProbeRay（64 B）
struct ProbeRay {
    DirectX::XMFLOAT3 dir;        // 12  射线方向
    float             hitT;       // 4   命中距离；miss = TMax
    DirectX::XMFLOAT3 hitPos;     // 12  世界空间命中点
    uint32_t          instanceID; // 4   命中实例；0xFFFFFFFF = miss
    DirectX::XMFLOAT3 hitN;       // 12  世界空间法线
    float             backface;   // 4   做探针是否在物体内部的检测
    DirectX::XMFLOAT4 radiance;   // 16  由 ProbeRayShadeCS 填写
};
static_assert(sizeof(ProbeRay) == 64);


// 对应 ShadersProbe.hlsl 的 ProbeUpdateParams（b4，16 DWORD = 64 B）
struct ProbeUpdateParams {
    // 网格
    float    originX, originY, originZ, spacing;          // 0
    uint32_t countX, countY, countZ, frameIndex;          // 16

    // 探针更新
    uint32_t probeCount;      // 探针总数
    uint32_t rayCount;        // 预留（Fibonacci 射线数）
    uint32_t objectIndex;     // GPUObjectData bindless SRV
    uint32_t irradianceUav;   // 辐照度 atlas UAV

    // 距离矩 atlas（D3 新增）
    uint32_t distanceUav;     // 距离矩 atlas UAV

    // border fixup 通用参数
    uint32_t borderTargetUav;   // 本次 fixup 的目标 atlas UAV
    uint32_t borderTileStride;  // 10（irradiance）或 18（distance）
    uint32_t rayBufferUav;      // raytrace pass的UAV handle
};
static_assert(sizeof(ProbeUpdateParams) == 64);