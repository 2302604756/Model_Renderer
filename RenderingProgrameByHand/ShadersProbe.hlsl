#include "ShadersCommon.hlsl"

struct ProbeUpdateParams
{
    float originX, originY, originZ, spacing; // 0
    uint countX, countY, countZ, frameIndex; // 16
    uint probeCount, rayCount, objectIndex, irradianceUav; // 32
    uint distanceUav; // 48
    uint borderTargetUav; // 52
    uint borderTileStride; // 56
    uint rayBufferUav; // 60
};
ConstantBuffer<ProbeUpdateParams> g_Probe : register(b4);

// fixed rays：方向每帧固定（只随探针旋转一次），只用于 relocation / classification
#define PROBE_FIXED_RAY_COUNT 0u

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


// 按 RTXGI 官方规则填 gutter：
// 上下行：内部同一行 + x 镜像；左右列：内部同一列 + y 镜像；四角：对角内部角
// 只读 tile 内部（1..I），只写 gutter，无竞争
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

    int2 p = int2(id.xy);
    int2 tileOrigin = (p / int(stride)) * int(stride);
    int2 local = p % int(stride);
    int I = int(stride) - 2; // 内部边长：irradiance=8，distance=16

    // 内部 texel 不处理
    if (local.x > 0 && local.x < I + 1 &&
        local.y > 0 && local.y < I + 1)
        return;

    RWTexture2D<float4> atlas = ResourceDescriptorHeap[target];

    bool left = (local.x == 0);
    bool right = (local.x == I + 1);
    bool top = (local.y == 0);
    bool bottom = (local.y == I + 1);

    int2 srcLocal;

    // 四个角：对角内部角
    if (top && left)
        srcLocal = int2(I, I); // TL <- BR
    else if (top && right)
        srcLocal = int2(1, I); // TR <- BL
    else if (bottom && left)
        srcLocal = int2(I, 1); // BL <- TR
    else if (bottom && right)
        srcLocal = int2(1, 1); // BR <- TL

    // 上下行：内部同一行，x 镜像
    else if (top)
        srcLocal = int2(I + 1 - local.x, 1);
    else if (bottom)
        srcLocal = int2(I + 1 - local.x, I);

    // 左右列：内部同一列，y 镜像
    else if (left)
        srcLocal = int2(1, I + 1 - local.y);
    else /* right */
        srcLocal = int2(I, I + 1 - local.y);

    atlas[p] = atlas[tileOrigin + srcLocal];
}

// 无效 probe 从最近 valid 邻居复制 atlas，避免动态物体经过时 cage 全失效
// 一个 group = 一个 probe，16×16 线程
// SV_GroupID是numthreadsCount * DispatchCount的index
// SV_GroupThreadID 是单个numthreads里的index
[numthreads(16, 16, 1)]
void ProbeFixupCS(uint3 groupId : SV_GroupID,
                  uint3 gtid : SV_GroupThreadID)
{
    uint3 counts = uint3(g_Probe.countX, g_Probe.countY, g_Probe.countZ);
    uint probeIndex = groupId.x;
    if (probeIndex >= counts.x * counts.y * counts.z)
        return;

    // 下面三个参数是Probe在空间中的index
    uint px = probeIndex % counts.x;                
    uint py = (probeIndex / counts.x) % counts.y;
    uint pz = probeIndex / (counts.x * counts.y);

    const uint irrStride = 10;
    const uint distStride = 18;

    // 计算当前thread对应大图集中的哪一个texel
    uint2 irrOrigin = uint2(
        px * irrStride,
        (py + pz * counts.y) * irrStride);

    uint2 distOrigin = uint2(
        px * distStride,
        (py + pz * counts.y) * distStride);

    RWTexture2D<float4> irrAtlas =
        ResourceDescriptorHeap[g_Probe.irradianceUav];

    RWTexture2D<float4> distAtlas =
        ResourceDescriptorHeap[g_Probe.distanceUav];

    // 读当前 probe 的 validity 中心 texel
    float selfValid = distAtlas[distOrigin + uint2(9, 9)].z;
    if (selfValid > 0.5f)
        return; // 本身有效，不需要 fixup

    StructuredBuffer<uint> probeMasks = ResourceDescriptorHeap[probeMaskIndex];
    uint selfMask = probeMasks[probeIndex];

    // 在 3×3×3 邻居里找最近的 valid probe
    int3 base = int3(px, py, pz);
    uint srcProbe = 0xFFFFFFFFu;
    float bestDist = 1e9f;

    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
            {
                int3 n = base + int3(dx, dy, dz);
                // 越界判断
                if (n.x < 0 || n.y < 0 || n.z < 0 ||
            n.x >= int(counts.x) ||
            n.y >= int(counts.y) ||
            n.z >= int(counts.z))
                    continue;

                uint nIndex = (n.z * counts.y + n.y) * counts.x + n.x;

                // 只复制同一 mask 的 probe
                if ((probeMasks[nIndex] & selfMask) == 0u)
                    continue;

                uint2 nDistOrigin = uint2( n.x * distStride, (n.y + n.z * counts.y) * distStride);

                // 只去看中间的那个texel就可以大致判断整体
                float nValid = distAtlas[nDistOrigin + uint2(9, 9)].z;
                if (nValid < 0.5f)
                    continue;

                // 去寻找最近的符合条件的邻居
                float d2 = float(dx * dx + dy * dy + dz * dz);
                if (d2 < bestDist)
                {
                    bestDist = d2;
                    srcProbe = nIndex;
                }
            }

    if (srcProbe == 0xFFFFFFFFu)
        return; // 周围也没有有效 probe

    // 目标Probe的空间index
    uint sx = srcProbe % counts.x;
    uint sy = (srcProbe / counts.x) % counts.y;
    uint sz = srcProbe / (counts.x * counts.y);

    uint2 srcIrrOrigin = uint2(
        sx * irrStride,
        (sy + sz * counts.y) * irrStride);

    uint2 srcDistOrigin = uint2(
        sx * distStride,
        (sy + sz * counts.y) * distStride);

    uint t = gtid.y * 16 + gtid.x;

    // 复制 irradiance 10×10（含 gutter）
    if (t < 100)
    {
        uint lx = t % 10;
        uint ly = t / 10;
        irrAtlas[irrOrigin + uint2(lx, ly)] = irrAtlas[srcIrrOrigin + uint2(lx, ly)];
    }

    // 复制 distance 18×18（324 个 texel，16×16=256 线程，循环两次）
    for (uint k = t; k < 324; k += 256)
    {
        uint lx = k % 18;
        uint ly = k / 18;

        float4 v = distAtlas[srcDistOrigin + uint2(lx, ly)];

        // 0.5 = fixup 置信度，既参与采样又不会完全冒充真实 probe
        v.z = 0.5f;

        distAtlas[distOrigin + uint2(lx, ly)] = v;
    }
}



// 1. ProbeRayCS：每个线程一条 Fibonacci 射线
//    线程总数 = probeCount * rayCount
[numthreads(64, 1, 1)]
void ProbeRayCS(uint3 id : SV_DispatchThreadID)
{
    uint3 counts = uint3(g_Probe.countX, g_Probe.countY, g_Probe.countZ);
    uint probeCount = g_Probe.probeCount;
    uint rayCount = g_Probe.rayCount;
    uint frameIndex = g_Probe.frameIndex;

    uint globalIndex = id.x;
    uint probeIndex = globalIndex / max(rayCount, 1u);
    uint rayIndex = globalIndex % max(rayCount, 1u);
    if (probeIndex >= probeCount)
        return;

    // 探针世界坐标
    uint px = probeIndex % counts.x;
    uint py = (probeIndex / counts.x) % counts.y;
    uint pz = probeIndex / (counts.x * counts.y);

    // 探针世界坐标 = 格点 + relocation 偏移
    // 必须和 SampleDDGI 用同一个位置，否则 atlas 描述的位置和查询位置错配
    RWStructuredBuffer<float4> probeData = ResourceDescriptorHeap[probeDataUavIndex];
    float3 probePos = float3(g_Probe.originX, g_Probe.originY, g_Probe.originZ)
                    + float3(px, py, pz) * g_Probe.spacing
                    + probeData[probeIndex].xyz;

    // 每探针 + 每帧随机旋转的 Fibonacci 方向
    // fixed ray不参与旋转
    const uint fixedRays = PROBE_FIXED_RAY_COUNT;
    const uint randomRays = rayCount - fixedRays;

    float2 baseRot = float2(
    Hash01(uint2(px, py), probeIndex),
    Hash01(uint2(pz, py), probeIndex + 1u));

    float3 dir;
    if (rayIndex < fixedRays)
    {
    // fixed rays：方向不随 frameIndex 变化，给 relocation 用
        dir = StochasticFibonacci(rayIndex, fixedRays, baseRot);
    }
    else
    {
        float2 rot = frac(baseRot + float2(
        frameIndex * 0.6180339887f,
        frameIndex * 0.754877666f));

        dir = StochasticFibonacci(
        rayIndex - fixedRays,
        randomRays,
        rot);
    }

    // 射线求交
    RayDesc ray;
    ray.Origin = probePos + dir * 0.02f;
    ray.Direction = dir;
    ray.TMin = 0.0f;
    ray.TMax = 100.0f;

    RayQuery < RAY_FLAG_NONE > query;
    query.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0xFF, ray);
    query.Proceed();

    ProbeRay outRay;
    outRay.dir = dir;
    outRay.radiance = float4(0.0f, 0.0f, 0.0f, 0.0f); // 留给 shade pass
    outRay.backface = 0.0f; // 默认正面；命中/ miss 分支会再覆盖

    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        uint instanceID = query.CommittedInstanceID();
        uint primID = query.CommittedPrimitiveIndex();
        float2 bary = query.CommittedTriangleBarycentrics();

        float3 hitPos, hitN, albedo;
        bool backface; //是否在物体内部的探测  mask
        FetchHitSurface(instanceID, primID, bary, g_Probe.objectIndex,
                dir, hitPos, hitN, albedo, backface);

        outRay.hitT = query.CommittedRayT();
        outRay.hitPos = hitPos;
        outRay.instanceID = instanceID;
        outRay.hitN = hitN;
        outRay.backface = backface ? 1.0f : 0.0f;
    }
    else
    {
        // miss：记一个"很远"的距离，radiance 由 shade pass 填天空色
        outRay.hitT = ray.TMax;
        outRay.hitPos = ray.Origin + dir * ray.TMax;
        outRay.instanceID = 0xFFFFFFFFu;
        outRay.hitN = float3(0.0f, 0.0f, 1.0f);
        outRay.backface = 0.0f;
    }

    RWStructuredBuffer<ProbeRay> rays = ResourceDescriptorHeap[g_Probe.rayBufferUav];
    rays[globalIndex] = outRay;
}

// 2. ProbeRayShadeCS：读 ray buffer，写 radiance 槽
[numthreads(64, 1, 1)]
void ProbeRayShadeCS(uint3 id : SV_DispatchThreadID)
{
    uint total = g_Probe.probeCount * g_Probe.rayCount;
    if (id.x >= total)
        return;
    
    // 如果是前32根rays则不参与irradiance计算
    uint rayIndex = id.x % g_Probe.rayCount;
    if (rayIndex < PROBE_FIXED_RAY_COUNT)
        return;

    RWStructuredBuffer<ProbeRay> rays = ResourceDescriptorHeap[g_Probe.rayBufferUav];
    ProbeRay r = rays[id.x];

    float3 radiance;
    if (r.instanceID == 0xFFFFFFFFu)
    {
        radiance = SkyColor();
    }
    else
    {
        // 命中点着色：直接光 + 天空环境
        StructuredBuffer<GPUObjectData> objects = ResourceDescriptorHeap[g_Probe.objectIndex];
        float3 albedo = objects[r.instanceID].albedo.rgb;

        // 天空可见性：从命中点朝开口方向打一条阴影射线
        float3 skyDir = normalize(float3(0.0f, 0.3f, -1.0f)); // 朝敞开的前方
        float skyVis = TraceSunShadow(r.hitPos, r.hitN, skyDir);

        radiance = ShadeHitPoint(r.hitPos, r.hitN, albedo);
    }

    rays[id.x].radiance = float4(radiance, 1.0f);
}

// ProbeTexelUpdateCS：用 ray buffer 加权更新 irradiance atlas
// 一个 group = 一个探针，8×8 线程 = 64 个 texel
[numthreads(8, 8, 1)]
void ProbeTexelUpdateCS(uint3 id : SV_DispatchThreadID)
{
    uint3 counts = uint3(g_Probe.countX, g_Probe.countY, g_Probe.countZ);
    uint probeCount = g_Probe.probeCount;
    uint rayCount = g_Probe.rayCount;
    uint frameIndex = g_Probe.frameIndex;

    uint probeIndex = id.x / 8;
    if (probeIndex >= probeCount)
        return;

    uint2 texel = uint2(id.x % 8, id.y);

    uint px = probeIndex % counts.x;
    uint py = (probeIndex / counts.x) % counts.y;
    uint pz = probeIndex / (counts.x * counts.y);

    // texel 中心方向
    float2 uv = (float2(texel) + 0.5f) / 8.0f;
    float3 texelDir = OctDecode(uv);

    // 遍历该探针的 rayCount 条射线，做余弦加权
    RWStructuredBuffer<ProbeRay> rays = ResourceDescriptorHeap[g_Probe.rayBufferUav];

    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float wsum = 0.0f;
    uint base = probeIndex * rayCount;

    for (uint r = PROBE_FIXED_RAY_COUNT; r < rayCount; ++r)
    {
        ProbeRay ray = rays[base + r];
        float w = max(0.0f, dot(texelDir, ray.dir));
        sum += ray.radiance.rgb * w;
        wsum += w;
    }

    float3 irradiance = sum / max(wsum, 1e-4f);
    
    // 写 irradiance atlas（stride 10）
    const uint stride = 10;
    uint2 pixel = uint2(
        px * stride + 1 + texel.x,
        (py + pz * counts.y) * stride + 1 + texel.y);

    RWTexture2D<float4> atlas = ResourceDescriptorHeap[g_Probe.irradianceUav];
    float3 old = atlas[pixel].rgb;
    float3 diff = abs(irradiance - old);
    float alpha = (frameIndex == 0) ? 1.0f : 0.05f;
    if (max(diff.r, max(diff.g, diff.b)) > 0.15f) alpha = 0.5f;
    atlas[pixel] = float4(lerp(old.rgb, irradiance, alpha), 1.0f);
}

// ProbeDistanceMomentsCS：用同一批 ray 的 hitT 更新距离矩 atlas
// 一个 group = 一个探针，16×16 线程 = 256 个 texel

// 探针有效性：0 = 在几何体内部（无效），1 = 有效
// groupshared 只能声明在全局作用域：组内 1 个线程写入，barrier 后所有线程读取 线程组内共享内存同一个线程组共享
groupshared float g_probeValidTarget;

[numthreads(16, 16, 1)]
void ProbeDistanceMomentsCS(uint3 id : SV_DispatchThreadID,
                            uint3 groupId : SV_GroupID,
                            uint3 gtid : SV_GroupThreadID)
{
    uint3 counts = uint3(g_Probe.countX, g_Probe.countY, g_Probe.countZ);
    uint rayCount = g_Probe.rayCount;
    uint frameIndex = g_Probe.frameIndex;

    uint probeIndex = groupId.x;
    if (probeIndex >= counts.x * counts.y * counts.z)
        return;

    uint px = probeIndex % counts.x;
    uint py = (probeIndex / counts.x) % counts.y;
    uint pz = probeIndex / (counts.x * counts.y);

    // 探针世界坐标 = grid 中心 + relocation offset
    RWStructuredBuffer<float4> probeData =
        ResourceDescriptorHeap[probeDataUavIndex];

    float3 probePos =
        float3(g_Probe.originX, g_Probe.originY, g_Probe.originZ)
        + float3(px, py, pz) * g_Probe.spacing
        + probeData[probeIndex].xyz;
    
    // texel 中心方向
    uint2 texel = gtid.xy;
    float2 uv = (float2(texel) + 0.5f) / 16.0f;
    float3 texelDir = OctDecode(uv);

    RWStructuredBuffer<ProbeRay> rays = ResourceDescriptorHeap[g_Probe.rayBufferUav];


    if (gtid.x == 0 && gtid.y == 0)
    {
        uint hits = 0;
        uint backfaces = 0;
        uint baseV = probeIndex * rayCount;

        for (uint r = 0; r < rayCount; ++r)
        {
            ProbeRay rr = rays[baseV + r];
            if (rr.instanceID == 0xFFFFFFFFu)
                continue;

            ++hits;
            if (rr.backface > 0.5f)
                ++backfaces;
        }

        

        // 几何体内部：背面比例高
        float hitRatio = (hits > 0) ? float(backfaces) / float(hits) : 0.0f;        // 打中背面的比例
        float targetValid = 1.0f;

         // 只有“大部分是命中且大部分是背面”才认为在物体内部
        if (hits > rayCount / 2)
        {
        // 0.55 以下视为外部，0.85 以上视为内部，中间平滑
            targetValid = 1.0f - smoothstep(0.55f, 0.85f, hitRatio);
        }

        g_probeValidTarget = targetValid;
    }
    
    //强制等待所有线程都写完才能继续前进
    GroupMemoryBarrierWithGroupSync();

    float targetValid = g_probeValidTarget;

    float sumD = 0.0f;
    float sumD2 = 0.0f;
    float wsum = 0.0f;
    uint base = probeIndex * rayCount;

    // 距离矩要比辐照度更锐的方向权重，否则 Chebyshev 会过度平滑
    const float depthSharpness = 50.0f;
    const float maxDistance = 4.0f;
    // fixed rays 只用于 relocation，不参与 distance moments 混合
    for (uint r = PROBE_FIXED_RAY_COUNT; r < rayCount; ++r)
    {
        ProbeRay ray = rays[base + r];

        // cosine-power 50次方权重
        float w = pow(max(0.0f, dot(texelDir, ray.dir)), depthSharpness);
        if (w < 1e-5f)
            continue;

        // miss 不跳过
        float d = (ray.instanceID == 0xFFFFFFFFu) ? maxDistance : ray.hitT;
        sumD += d * w;
        sumD2 += d * d * w;
        wsum += w;
    }

    float mean = (wsum > 1e-5f) ? (sumD / wsum) : maxDistance;
    float mean2 = (wsum > 1e-5f) ? (sumD2 / wsum) : maxDistance * maxDistance;

    const uint stride = 18;
    uint2 pixel = uint2(
        px * stride + 1 + texel.x,
        (py + pz * counts.y) * stride + 1 + texel.y);

    RWTexture2D<float4> distAtlas = ResourceDescriptorHeap[g_Probe.distanceUav];
    float4 old = distAtlas[pixel];
    float alpha = (frameIndex == 0u) ? 1.0f : 0.05f;

    
    float prevValid = old.z;
    // 失效快一点，恢复慢一点，避免鬼影
    float validAlpha = (frameIndex == 0) ? 1.0f : 0.05f;
    float valid = lerp(prevValid, targetValid, validAlpha);
    
    distAtlas[pixel] = float4(
        lerp(old.x, mean, alpha),
        lerp(old.y, mean2, alpha),
        valid, // z = 1 有效，0 无效
        1.0f);
}


// ---------------- 用于ProbeRelocation的CS ------------------
// 一个线程一个 probe
[numthreads(64, 1, 1)]
void ProbeRelocationCS(uint3 id : SV_DispatchThreadID)
{
    uint probeIndex = id.x;
    if (probeIndex >= g_Probe.probeCount)
        return;

    uint3 counts = uint3(g_Probe.countX, g_Probe.countY, g_Probe.countZ);
    uint px = probeIndex % counts.x;
    uint py = (probeIndex / counts.x) % counts.y;
    uint pz = probeIndex / (counts.x * counts.y);

    float3 center =
        float3(g_Probe.originX, g_Probe.originY, g_Probe.originZ)
        + float3(px, py, pz) * g_Probe.spacing;

    RWStructuredBuffer<float4> probeData =
        ResourceDescriptorHeap[probeDataUavIndex];

    RWStructuredBuffer<ProbeRay> rays =
        ResourceDescriptorHeap[g_Probe.rayBufferUav];

    float4 data = probeData[probeIndex];
    float3 offset = data.xyz;

    // 只用前 N 条固定方向射线：方向稳定，才能当"探针是否在实体里"的稳定判据
    const uint fixedRays = min(PROBE_FIXED_RAY_COUNT, g_Probe.rayCount);
    if (fixedRays == 0u)
        return;
    uint base = probeIndex * g_Probe.rayCount;

    int closestBackfaceIndex = -1;
    int closestFrontfaceIndex = -1;
    int farthestFrontfaceIndex = -1;

    float closestBackfaceDist = 1e27f;
    float closestFrontfaceDist = 1e27f;
    float farthestFrontfaceDist = 0.0f;
    float backfaceCount = 0.0f;

    for (uint r = 0; r < fixedRays; ++r)
    {
        ProbeRay rr = rays[base + r];

        bool isMiss = (rr.instanceID == 0xFFFFFFFFu);
        bool isBackface = (rr.backface > 0.5f);
        float d = rr.hitT;

        if (isBackface)
        {
            ++backfaceCount;
            if (d < closestBackfaceDist)
            {
                closestBackfaceDist = d;
                closestBackfaceIndex = r;
            }
        }
        else
        {
            if (!isMiss && d < closestFrontfaceDist)
            {
                closestFrontfaceDist = d;
                closestFrontfaceIndex = r;
            }

            if (d > farthestFrontfaceDist)
            {
                farthestFrontfaceDist = d;
                farthestFrontfaceIndex = r;
            }
        }
    }

    float3 fullOffset = offset;

    // 官方默认：probeMinFrontfaceDistance = 1.0（世界单位）、backface 阈值 = 0.25。
    // 本工程房间只有 5×2.5×5 m、spacing=0.5，按间距比例给 0.5 起步；
    // 想让探针更"远离墙面"就调到 0.75 / 1.0，代价是移动幅度更容易顶到椭球上限。
    const float minFrontfaceDistance = g_Probe.spacing * 0.5f;
    const float backfaceThreshold = 0.25f;

    if (closestBackfaceIndex != -1 &&
        (backfaceCount / float(fixedRays)) > backfaceThreshold)
    {
        // A. 在几何体内部，沿最近 backface 移出
        float3 dir = rays[base + closestBackfaceIndex].dir;
        fullOffset = offset +
            dir * (closestBackfaceDist + minFrontfaceDistance * 0.5f);
    }
    else if (closestFrontfaceIndex != -1 &&
             closestFrontfaceDist < minFrontfaceDistance)
    {
        // B. 离 frontface 太近，向最远 frontface 方向移动
        float3 closestDir = rays[base + closestFrontfaceIndex].dir;
        float3 farthestDir = rays[base + farthestFrontfaceIndex].dir;

        if (dot(closestDir, farthestDir) <= 0.0f)
        {
            farthestDir *= min(farthestFrontfaceDist, 1.0f);
            fullOffset = offset + farthestDir;
        }
    }
    else if (closestFrontfaceDist > minFrontfaceDistance)
    {
        // C. 附近没有几何，offset 向 0 回拉
        float offsetLen = length(offset);
        if (offsetLen > 1e-5f)
        {
            float margin = min(
                closestFrontfaceDist - minFrontfaceDistance,
                offsetLen);

            fullOffset = offset - normalize(offset) * margin;
        }
    }

    // 限制在自己的 grid cell：0.45 × spacing
    float3 normalizedOffset = fullOffset / g_Probe.spacing;
    if (dot(normalizedOffset, normalizedOffset) < 0.2025f)
    {
        offset = fullOffset;
    }

    // 注意：本工程存的是**世界空间**偏移；RTXGI 存的是 offset/spacing 的归一化值。
    // 读写两侧都在本文件与 ShadersLighting.hlsl 里，口径一致即可，不要混着抄。
    probeData[probeIndex] = float4(offset, data.w);
}