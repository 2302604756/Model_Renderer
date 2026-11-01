#pragma once
#include <cstdint>

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
    uint32_t reserved0;         // 未使用，保持 16 DWORD
};
static_assert(sizeof(ProbeUpdateParams) == 64);