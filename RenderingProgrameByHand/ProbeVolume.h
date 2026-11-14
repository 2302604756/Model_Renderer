#pragma once
#include <DirectXMath.h>
#include <cstdint>

// 一个体积对应一个 bit；probe 和 surface 用 bitmask 匹配
//规定一个探针立方体区域
struct ProbeVolume {
    DirectX::XMFLOAT3 boundsMin{ -2.45f, 0.0f, -2.5f };
    DirectX::XMFLOAT3 boundsMax{ 2.45f, 2.45f, 1.95f };
    uint32_t          mask = 1u;
};