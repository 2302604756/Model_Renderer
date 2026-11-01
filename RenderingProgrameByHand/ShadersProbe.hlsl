#include "ShadersCommon.hlsl"

struct ProbeUpdateParams
{
    float originX, originY, originZ, spacing; // 0
    uint countX, countY, countZ, frameIndex; // 16
    uint probeCount, rayCount, objectIndex, irradianceUav; // 32
    uint distanceUav; // 48
    uint borderTargetUav; // 52
    uint borderTileStride; // 56
    uint reserved0; // 60
};
ConstantBuffer<ProbeUpdateParams> g_Probe : register(b4);

// SV_DispatchThreadID是从0-63
[numthreads(8, 8, 1)]
void ProbeUpdateCS(uint3 id : SV_DispatchThreadID)
{
    // 新结构体字段（和 ProbeTypes.h 一致）
    uint3 counts = uint3(g_Probe.countX, g_Probe.countY, g_Probe.countZ);
    uint probeCount = g_Probe.probeCount;
    uint frameIndex = g_Probe.frameIndex;

    uint probeIndex = id.x / 8;
    if (probeIndex >= probeCount)
        return;

    uint2 texel = uint2(id.x % 8, id.y);

    uint px = probeIndex % counts.x;
    uint py = (probeIndex / counts.x) % counts.y;
    uint pz = probeIndex / (counts.x * counts.y);

    float3 probePos = float3(g_Probe.originX, g_Probe.originY, g_Probe.originZ)
                    + float3(px, py, pz) * g_Probe.spacing;

    // texel 中心方向
    float2 uv = (float2(texel) + 0.5f) / 8.0f;
    float3 dir = OctDecode(uv);

    // 沿该方向 trace
    float3 radiance = TraceRadiance(probePos + dir * 0.02f, dir);

    const uint stride = 10; // irradiance tile: 8 + 2*1

    uint2 pixel = uint2(
        px * stride + 1 + texel.x,
        (py + pz * counts.y) * stride + 1 + texel.y);

    RWTexture2D<float4> atlas = ResourceDescriptorHeap[g_Probe.irradianceUav];

    float4 old = atlas[pixel];
    float alpha = (frameIndex == 0u) ? 1.0f : 0.05f;
    atlas[pixel] = float4(lerp(old.rgb, radiance, alpha), 1.0f);
}

// 距离矩更新：每个 group = 一个探针，16×16 线程 = 256 个方向 texel
// 写入 (mean distance, mean distance²)
[numthreads(16, 16, 1)]
void ProbeDistanceCS(uint3 id : SV_DispatchThreadID,
                     uint3 groupId : SV_GroupID,
                     uint3 gtid : SV_GroupThreadID)
{
    uint3 counts = uint3(g_Probe.countX, g_Probe.countY, g_Probe.countZ);
    uint frameIndex = g_Probe.frameIndex;

    // 每个 group 负责一个探针
    uint probeIndex = groupId.x;
    if (probeIndex >= counts.x * counts.y * counts.z)
        return;

    uint px = probeIndex % counts.x;
    uint py = (probeIndex / counts.x) % counts.y;
    uint pz = probeIndex / (counts.x * counts.y);

    // 探针世界坐标
    float3 probePos = float3(g_Probe.originX, g_Probe.originY, g_Probe.originZ)
                    + float3(px, py, pz) * g_Probe.spacing;

    // 当前线程负责的距离 texel
    uint2 texel = gtid.xy; // 0..15
    float2 uv = (float2(texel) + 0.5f) / 16.0f;
    float3 dir = OctDecode(uv);

    // miss 用 100 表示"很远"，Chebyshev 会当作可见
    float t = TraceHitDistance(probePos + dir * 0.02f, dir, 100.0f);

    const uint stride = 18; // 16 + 2*1 gutter

    uint2 pixel = uint2(
        px * stride + 1 + texel.x,
        (py + pz * counts.y) * stride + 1 + texel.y);

    RWTexture2D<float4> distAtlas = ResourceDescriptorHeap[g_Probe.distanceUav];

    float4 old = distAtlas[pixel];
    float alpha = (frameIndex == 0u) ? 1.0f : 0.05f;

    float mean = lerp(old.x, t, alpha); // E[d]
    float mean2 = lerp(old.y, t * t, alpha); // E[d²]
    distAtlas[pixel] = float4(mean, mean2, 0.0f, 1.0f);
}


// 把 gutter 填成该 tile 边缘的重复
// 只读 tile 内部（1..8），只写 gutter（0/9），无竞争
[numthreads(8, 8, 1)]
void BorderFixupCS(uint3 id : SV_DispatchThreadID)
{
    uint3 counts = uint3(g_Probe.countX, g_Probe.countY, g_Probe.countZ);

    uint stride = g_Probe.borderTileStride; // 10 或 18
    uint target = g_Probe.borderTargetUav; // 目标 atlas 的 UAV 槽

    uint atlasW = counts.x * stride;
    uint atlasH = counts.y * counts.z * stride;
    if (id.x >= atlasW || id.y >= atlasH)
        return;

    // 当前像素属于哪个 tile、tile 内坐标
    uint2 p = id.xy;
    int2 tile = int2(p) / int(stride);
    int2 local = int2(p) % int(stride);

    // 只处理最外一圈 gutter
    bool gutterX = (local.x == 0) || (local.x == int(stride) - 1);
    bool gutterY = (local.y == 0) || (local.y == int(stride) - 1);
    if (!gutterX && !gutterY)
        return;

    RWTexture2D<float4> atlas = ResourceDescriptorHeap[target];

    // 当前 tile 在 atlas 中的起点
    int2 tileOrigin = tile * int(stride);

    // gutter 向 tile 内部偏移 1 像素做镜像
    int maskX = gutterX ? ((local.x == 0) ? 1 : -1) : 0;
    int maskY = gutterY ? ((local.y == 0) ? 1 : -1) : 0;

    int2 dstPix = tileOrigin + local;
    int2 srcPix = dstPix + int2(maskX, maskY);

    atlas[dstPix] = atlas[srcPix];
}