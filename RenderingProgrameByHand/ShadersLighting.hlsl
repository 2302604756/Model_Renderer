#include "ShadersCommon.hlsl"
#include "GiPassBindings.hlsli"

// =============================================================================
// Lighting pass：TraceGI + 全屏三角形 + 最终着色
// =============================================================================

// 一次弹射的间接光：u 是算好的低差异 2D 样本
float3 TraceGI(float3 worldPos, float3 N, float2 u)
{
    float3 dir = SampleCosineHemisphere(N, u.x, u.y);
    return TraceRadiance(worldPos + N * 0.01f, dir);
}

// 光追 GI：每像素多个余弦半球样本
float3 TraceGIMulti(float3 worldPos, float3 N, uint2 pixel)
{
    const uint samples = 16;
    float3 indirect = float3(0.0f, 0.0f, 0.0f);

    for (uint i = 0; i < samples; ++i)
    {
        uint sampleIndex = g_Frame.frameIndex * samples + i;
        float2 u = QuasiRandom2D(pixel, sampleIndex);
        indirect += TraceGI(worldPos, N, u);
    }

    return indirect / float(samples);
}

// 全屏三角形
struct LightingVSOut
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

LightingVSOut LightingVS(uint vertexID : SV_VertexID)
{
    LightingVSOut o;
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2); // (0,0) (2,0) (0,2)
    o.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    o.uv = uv;
    return o;
}


//============================================================================
//
// DDGI 采样
// 采样单个探针的 irradiance（octahedral 双线性）
float3 SampleProbeIrradiance(uint probeIndex, float3 dir)
{
    uint3 counts = g_Lighting.gridCountsAndProbeIndex.xyz;
    const uint stride = 10; // 8 + 2*1 gutter
    const uint tileSize = 8;
    
    // octahedral 方向 → tile 内像素坐标
    float2 uv = OctEncode(dir) * float(tileSize);
    
     // tile 内部左上角（跳过 gutter）
    uint2 tilePx = uint2(
        (probeIndex % counts.x) * stride + 1,
        (probeIndex / counts.x) * stride + 1);
    
     // 整张 atlas 大小
    float2 atlasSize = float2(
        counts.x * stride,
        counts.y * counts.z * stride);

    // 双线性：gutter 已镜像，边缘不会漏色
    float2 atlasUV = (float2(tilePx) + uv) / atlasSize;

    Texture2D<float4> atlas =
        ResourceDescriptorHeap[g_Lighting.gridCountsAndProbeIndex.w];

    return atlas.SampleLevel(g_ClampSampler, atlasUV, 0.0f).rgb;
}

// 采样单个探针的距离矩（x = E[d]，y = E[d²]，z = validity）
float3 SampleProbeDistance(uint probeIndex, float3 dir)
{
    uint3 counts = g_Lighting.gridCountsAndProbeIndex.xyz;

    const uint stride = 18; // 16 + 2*1 gutter
    const uint tileSize = 16;

    float2 uv = OctEncode(dir) * float(tileSize);

    uint2 tilePx = uint2(
        (probeIndex % counts.x) * stride + 1,
        (probeIndex / counts.x) * stride + 1);

    float2 atlasSize = float2(
        counts.x * stride,
        counts.y * counts.z * stride);

    float2 atlasUV = (float2(tilePx) + uv) / atlasSize;

    Texture2D<float4> distAtlas =
        ResourceDescriptorHeap[g_Lighting.distanceAtlasIndex];

    return distAtlas.SampleLevel(g_ClampSampler, atlasUV, 0.0f).xyz;
}

// DDGI 采样结果 + 调试信息
//用于在debug中展示
struct DDGISample
{
    float3 E; // 归一化后的 E/π
    float wsum; // 8 个探针的总权重
    float validCount; // 有效探针数（0~8）
    float chebAvg; // 加权平均 Chebyshev 可见性
    uint dominantProbe; // 权重最大的探针 index
};

// Probe的一维Index
uint DDGIProbeIndex(int3 coords, uint3 counts)
{
    return (coords.z * counts.y + coords.y) * counts.x + coords.x;
}

// 生成对应角点的索引
int3 DDGICellCorner(uint i)
{
    return int3(i & 1u, (i >> 1) & 1u, (i >> 2) & 1u);
}

// 
float DDGITrilinearWeight(int3 corner, float3 cellFraction)
{
    float3 axisWeights = lerp(1.0f - cellFraction, cellFraction, float3(corner));
    return axisWeights.x * axisWeights.y * axisWeights.z;
}

float DDGIWrapWeight(float3 pointToProbe, float3 normal)
{
    float backFacing = saturate(-dot(normalize(pointToProbe), normal));
    return 1.0f - 0.8f * backFacing * backFacing;
}

float DDGIChebyshevWeight(float2 moments, float distanceToProbe)
{
    const float varianceBias = 0.02f;
    const float meanBias = 0.07f;
    float variance = abs(moments.x * moments.x - moments.y) + varianceBias;
    float mean = max(moments.x + meanBias, 0.0f);

    float visibility = 1.0f;
    if (distanceToProbe > mean)
    {
        float delta = distanceToProbe - mean;
        visibility = variance / (variance + delta * delta);
    }

    return max(visibility * visibility * visibility, 0.05f);
}

DDGISample SampleDDGI(float3 P, float3 N, uint pointMask)
{
    StructuredBuffer<uint> probeMasks = ResourceDescriptorHeap[probeMaskIndex];
    RWStructuredBuffer<float4> probeData = ResourceDescriptorHeap[probeDataUavIndex];

    uint3 counts = g_Lighting.gridCountsAndProbeIndex.xyz;
    int3 maxProbeCoords = int3(counts) - 1;
    float3 origin = g_Lighting.gridOriginSpacing.xyz;
    float spacing = g_Lighting.gridOriginSpacing.w;

    // 插值使用规则格点。可见性和背面权重使用重定位后的探针位置。
    float3 gridPos = (P - origin) / spacing;
    float3 base = floor(gridPos);
    int3 baseCoords = int3(base);
    float3 cellFraction = saturate(gridPos - base);
    float3 smoothFraction = cellFraction * cellFraction * (3.0f - 2.0f * cellFraction);
    const float interpolationSmoothness = 0.0f;
    cellFraction = lerp(cellFraction, smoothFraction, interpolationSmoothness);

    float normalBias = spacing * 0.15f;
    float3 viewDirection = normalize(g_Frame.cameraPos.xyz - P);
    float3 visibilityBias = (N + 3.0f * viewDirection) * normalBias;
    bool trilinearOnly = g_Lighting.debugMode == 15;

    float3 irradianceSum = 0.0f;
    float weightSum = 0.0f;
    float validCount = 0.0f;
    float visibilitySum = 0.0f;
    float dominantWeight = -1.0f;
    uint dominantProbe = 0;

    // 主路径：累加当前格子的八个角点。
    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        int3 corner = DDGICellCorner(i);
        int3 coords = clamp(baseCoords + corner, int3(0, 0, 0), maxProbeCoords);
        uint probeIndex = DDGIProbeIndex(coords, counts);
        bool maskMatch = (probeMasks[probeIndex] & pointMask) != 0u;
        float trilinearWeight = DDGITrilinearWeight(corner, cellFraction);

        float3 probePos = origin + float3(coords) * spacing + probeData[probeIndex].xyz;
        float3 probeToBiasedPoint = P - probePos + visibilityBias;
        float distanceToProbe = length(probeToBiasedPoint) + 1e-4f;
        float3 moments = SampleProbeDistance(probeIndex, probeToBiasedPoint / distanceToProbe);

        if (moments.z > 0.5f)
            validCount += 1.0f;

        float visibility = DDGIChebyshevWeight(moments.xy, distanceToProbe);
        float weight = DDGIWrapWeight(probePos - P, N);
        weight *= moments.z;
        weight *= visibility;
        weight = max(1e-6f, weight);

        // 先压低质量较差的探针权重，再乘格内插值权重。
        const float crushThreshold = 0.05f;
        if (weight < crushThreshold)
            weight *= (weight * weight) * (1.0f / (crushThreshold * crushThreshold));

        if (!maskMatch)
            weight = 0.0f;

        weight *= trilinearWeight;
        visibilitySum += visibility * weight;

        // 调试模式 15 用格内权重单独显示辐照度。
        if (trilinearOnly)
            weight = trilinearWeight;

        if (weight > dominantWeight)
        {
            dominantWeight = weight;
            dominantProbe = probeIndex;
        }

        irradianceSum += SampleProbeIrradiance(probeIndex, N) * weight;
        weightSum += weight;
    }

    // 低权重时先重试当前八个角点，再查周围 27 个格点。
    const float fallbackThreshold = 0.02f;
    const float minFallbackWeight = 1e-5f;
    if (weightSum < fallbackThreshold)
    {
        float3 fallbackIrradiance = 0.0f;
        float fallbackWeight = 0.0f;

        // Fixup 补过的辐照度可以参与插值，此处只应用 mask、朝向和格内权重。
        [unroll]
        for (uint i = 0; i < 8; ++i)
        {
            int3 corner = DDGICellCorner(i);
            int3 coords = clamp(baseCoords + corner, int3(0, 0, 0), maxProbeCoords);
            uint probeIndex = DDGIProbeIndex(coords, counts);
            if ((probeMasks[probeIndex] & pointMask) == 0u)
                continue;

            float3 probePos = origin + float3(coords) * spacing + probeData[probeIndex].xyz;
            float weight = DDGIWrapWeight(probePos - P, N) * DDGITrilinearWeight(corner, cellFraction);
            fallbackIrradiance += SampleProbeIrradiance(probeIndex, N) * weight;
            fallbackWeight += weight;
        }

        if (!(fallbackWeight > minFallbackWeight))
        {
            fallbackIrradiance = 0.0f;
            fallbackWeight = 0.0f;

            [unroll]
            for (int i = 0; i < 27; ++i)
            {
                int3 offset = int3(i % 3, (i / 3) % 3, i / 9) - 1;
                int3 coords = clamp(baseCoords + offset, int3(0, 0, 0), maxProbeCoords);
                uint probeIndex = DDGIProbeIndex(coords, counts);
                if ((probeMasks[probeIndex] & pointMask) == 0u)
                    continue;

                float3 probePos = origin + float3(coords) * spacing + probeData[probeIndex].xyz;
                float3 probeToPoint = P - probePos;
                float distanceToProbe = max(length(probeToPoint), 1e-3f);
                // 邻域兜底沿用未加偏置的点到探针方向。
                float3 moments = SampleProbeDistance(probeIndex, -probeToPoint / distanceToProbe);

                float weight = DDGIWrapWeight(probePos - P, N);
                weight *= moments.z;
                weight *= DDGIChebyshevWeight(moments.xy, distanceToProbe);
                weight *= 1.0f / (1.0f + distanceToProbe * distanceToProbe * 4.0f);

                fallbackIrradiance += SampleProbeIrradiance(probeIndex, N) * weight;
                fallbackWeight += weight;
            }
        }

        if (fallbackWeight > minFallbackWeight)
        {
            DDGISample fallback;
            fallback.E = fallbackIrradiance / fallbackWeight;
            fallback.wsum = 1.0f;
            fallback.validCount = 8.0f;
            fallback.chebAvg = 1.0f;
            fallback.dominantProbe = 0;
            return fallback;
        }
    }

    DDGISample result;
    result.E = weightSum < 1e-4f ? ambient.rgb : irradianceSum / max(weightSum, 1e-5f);
    result.wsum = weightSum;
    result.validCount = validCount;
    result.chebAvg = visibilitySum / max(weightSum, 1e-5f);
    result.dominantProbe = dominantProbe;
    return result;
}

// 探针 index → 彩色，用于 debug 14
float3 ProbeDebugColor(uint x)
{
    x = (x << 13) ^ x;
    x = x * (x * x * 15731u + 789221u) + 1376312589u;
    return float3(
        float(x & 0xFFu),
        float((x >> 8) & 0xFFu),
        float((x >> 16) & 0xFFu)) / 255.0f;
}


// 8-bit 输出抖动：打散 R8G8B8A8 后端的色阶断层
// 交错梯度噪声固定在屏幕空间，不随帧变化，不会引入时间闪烁
float Dither8(uint2 p)
{
    float ign = frac(52.9829189f * frac(0.06711056f * p.x + 0.00583715f * p.y));
    return (ign - 0.5f) / 255.0f;
}

float4 LightingPS(LightingVSOut pin) : SV_Target
{
    Texture2D<float4> albedoTex = ResourceDescriptorHeap[g_Lighting.albedoIndex];
    Texture2D<float4> normalTex = ResourceDescriptorHeap[g_Lighting.normalIndex];
    Texture2D<float> depthTex = ResourceDescriptorHeap[g_Lighting.depthIndex];

    uint2 pixel = uint2(pin.position.xy);

    Texture2D<uint> volumeMaskTex = ResourceDescriptorHeap[volumeMaskIndex];

    uint pointMask = volumeMaskTex.Load(int3(pixel, 0)).r;
    float4 albedo = albedoTex.SampleLevel(g_LinearSampler, pin.uv, 0.0f);
    float3 N = normalize(normalTex.SampleLevel(g_LinearSampler, pin.uv, 0.0f).xyz);
    float depth = depthTex.Load(int3(pixel, 0));

    // ---- debug 视图 ----
    if (g_Lighting.debugMode == 2)
        return float4(N * 0.5f + 0.5f, 1.0f);

    // ---- depth -> 世界坐标 ----
    float2 ndc = float2(pin.uv.x * 2.0f - 1.0f, 1.0f - pin.uv.y * 2.0f);
    float4 world = mul(g_Frame.invViewProj, float4(ndc, depth, 1.0f));
    world.xyz /= world.w;

    // debug 16 / 18：光追 GI 的 final / only
    if (g_Lighting.debugMode == 16 || g_Lighting.debugMode == 18)
    {
        float3 indirect = TraceGIMulti(world.xyz, N, pixel);

        if (g_Lighting.debugMode == 18)
            return float4(albedo.rgb * indirect, 1.0f);

        float3 directRT = EvaluateDirectLighting(world.xyz, N);
        return float4(albedo.rgb * (directRT + ambient.rgb + indirect), 1.0f);
    }

    
    // debug 9：atlas 1:1 显示在左上角（验证 tile 布局）
    if (g_Lighting.debugMode == 9)
    {
        uint3 counts = g_Lighting.gridCountsAndProbeIndex.xyz;
        const uint stride = 10;
        uint atlasW = counts.x * stride;
        uint atlasH = counts.y * counts.z * stride;

        uint2 p = uint2(pin.position.xy) / 5; // 2x 放大
        if (p.x >= atlasW || p.y >= atlasH)
            return float4(0.0f, 0.0f, 0.0f, 1.0f);

        Texture2D<float4> atlas = ResourceDescriptorHeap[g_Lighting.gridCountsAndProbeIndex.w];
        float3 c = atlas.Load(int3(p, 0)).rgb;

        if ((p.x % stride) == 0 || (p.y % stride) == 0)
            c = float3(1.0f, 0.0f, 1.0f); // tile 边界（gutter 起点）

        return float4(c, 1.0f);
    }
    
        // debug 10：距离矩 atlas（x = mean dist，y = mean dist²）
    if (g_Lighting.debugMode == 10)
    {
        uint3 counts = g_Lighting.gridCountsAndProbeIndex.xyz;
        const uint stride = 18; // 16 + 2*1 gutter

        // 4 倍缩小显示
        uint2 p = uint2(pin.position.xy) / 4;
        uint w = counts.x * stride;
        uint h = counts.y * counts.z * stride;
        if (p.x >= w || p.y >= h)
            return float4(0, 0, 0, 1);

        Texture2D<float4> distAtlas = ResourceDescriptorHeap[g_Lighting.distanceAtlasIndex];
        float4 m = distAtlas.Load(int3(p, 0));

        // R = mean distance / 10，G = mean²/100，B = validity
        return float4(m.x / 10.0f, saturate(m.y / 100.0f), m.z, 1.0f);
    }

    DDGISample ddgi = SampleDDGI(world.xyz, N, pointMask);

    if (g_Lighting.debugMode == 8 || g_Lighting.debugMode == 15)
        return float4(ddgi.E, 1.0f);

    // debug 17：DDGI only（albedo × DDGI）
    if (g_Lighting.debugMode == 17)
        return float4(albedo.rgb * ddgi.E, 1.0f);

    // debug 11：8 个 cage 探针里有多少个有效（绿=8，红=0）
    if (g_Lighting.debugMode == 11)
    {
        float v = saturate(ddgi.validCount / 8.0f);
        return float4(1.0f - v, v, 0.0f, 1.0f);
    }

    // debug 12：加权平均 Chebyshev 可见性（绿=可见，红=被遮挡）
    if (g_Lighting.debugMode == 12)
    {
        float c = saturate(ddgi.chebAvg);
        return float4(1.0f - c, c, 0.0f, 1.0f);
    }

    // debug 13：8 个探针的总权重（越白越大）
    if (g_Lighting.debugMode == 13)
    {
        float t = saturate(ddgi.wsum);
        return float4(t, t, t, 1.0f);
    }

    // debug 14：权重最大的探针 index（看格子边界）
    if (g_Lighting.debugMode == 14)
        return float4(ProbeDebugColor(ddgi.dominantProbe), 1.0f);
    
        // debug 19：探针位置可视化（粉色小球）
    if (g_Lighting.debugMode == 19)
    {
        float3 direct = EvaluateDirectLighting(world.xyz, N);
        float3 color = albedo.rgb * (direct + ambient.rgb + ddgi.E);

        float3 gridOrigin = g_Lighting.gridOriginSpacing.xyz;
        float spacing = g_Lighting.gridOriginSpacing.w;
        uint3 counts = g_Lighting.gridCountsAndProbeIndex.xyz;

        float3 ro = g_Frame.cameraPos.xyz;
        float3 rd = normalize(world.xyz - ro);
        float sceneDist = length(world.xyz - ro);

        // 探针球半径和步长：半径约 spacing 的 0.18，步长略小于半径
        float radius = spacing * 0.18f;
        float stepLen = radius * 0.9f;

        bool hit = false;
        float3 hitNormal = float3(0.0f, 1.0f, 0.0f);

        // 沿射线步进，取最近的格点探针做球测试
        [loop]
        for (uint stepIndex = 0; stepIndex < 512; ++stepIndex)
        {
            float t = float(stepIndex) * stepLen;
            if (t >= sceneDist)
                break;

            float3 p = ro + rd * t;
            int3 idx = int3(round((p - gridOrigin) / spacing));

            if (any(idx < int3(0, 0, 0)) || any(idx >= int3(counts)))
                continue;

            float3 probePos = gridOrigin + float3(idx) * spacing;
            float3 d = p - probePos;

            if (dot(d, d) < radius * radius)
            {
                hit = true;
                hitNormal = normalize(d);
                break;
            }
        }

        if (hit)
        {
            // 粉色 + 轻微明暗，看起来像小球
            float3 lightDir = normalize(float3(0.3f, 0.6f, -0.5f));
            float shade = 0.55f + 0.45f * saturate(dot(hitNormal, lightDir));
            color = float3(1.0f, 0.12f, 0.55f) * shade;
        }

        return float4(color, 1.0f);
    }

    // ---- final：直射 + 环境 + DDGI ----
    float3 direct = EvaluateDirectLighting(world.xyz, N);
    float3 color = albedo.rgb * (direct + ambient.rgb + ddgi.E);
    color = saturate(color);
    color += Dither8(pixel); // ±0.5 LSB 抖动，消除色阶断层
    return float4(color, 1.0f);
}

struct DeferredGiSurface
{
    float3 position;
    float3 normal;
    float3 albedo;
    bool valid;
};

DeferredGiSurface LoadDeferredGiSurface(LightingVSOut pin)
{
    DeferredGiSurface surface = (DeferredGiSurface) 0;
    Texture2D<float> depthTex = ResourceDescriptorHeap[g_Lighting.depthIndex];
    float depth = depthTex.Load(int3(uint2(pin.position.xy), 0));
    // 当前正向深度以 1 清屏。背景在标准 HDR 结果中写零。
    if (depth >= 1.0f)
        return surface;

    Texture2D<float4> albedoTex = ResourceDescriptorHeap[g_Lighting.albedoIndex];
    Texture2D<float4> normalTex = ResourceDescriptorHeap[g_Lighting.normalIndex];
    float2 ndc = pin.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    float4 world = mul(g_Frame.invViewProj, float4(ndc, depth, 1.0f));
    surface.position = world.xyz / world.w;
    surface.normal = normalize(normalTex.SampleLevel(g_LinearSampler, pin.uv, 0.0f).xyz);
    surface.albedo = albedoTex.SampleLevel(g_LinearSampler, pin.uv, 0.0f).rgb;
    surface.valid = true;
    return surface;
}

float4 DirectLightingPS(LightingVSOut pin) : SV_Target
{
    DeferredGiSurface surface = LoadDeferredGiSurface(pin);
    if (!surface.valid)
        return 0.0f;

    // 延续现有的直接光与环境项。中间纹理保留线性 HDR。
    float3 direct = EvaluateDirectLighting(surface.position, surface.normal);
    return float4(surface.albedo * (direct + ambient.rgb), 1.0f);
}

float4 DDGIResolvePS(LightingVSOut pin) : SV_Target
{
    DeferredGiSurface surface = LoadDeferredGiSurface(pin);
    if (!surface.valid)
        return 0.0f;

    Texture2D<uint> volumeMaskTex = ResourceDescriptorHeap[volumeMaskIndex];
    uint pointMask = volumeMaskTex.Load(int3(uint2(pin.position.xy), 0));
    DDGISample sample = SampleDDGI(surface.position, surface.normal, pointMask);
    return float4(surface.albedo * sample.E, 1.0f);
}

float4 RayTracedDiffusePS(LightingVSOut pin) : SV_Target
{
    DeferredGiSurface surface = LoadDeferredGiSurface(pin);
    if (!surface.valid)
        return 0.0f;

    float3 incoming = TraceGIMulti(surface.position, surface.normal, uint2(pin.position.xy));
    return float4(surface.albedo * incoming, 1.0f);
}

float4 CompositeLightingPS(LightingVSOut pin) : SV_Target
{
    int3 pixel = int3(uint2(pin.position.xy), 0);
    float3 color = 0.0f;

    if (g_GiDisplayMode == GiDisplayFull)
    {
        Texture2D<float4> directTex = ResourceDescriptorHeap[g_DirectLightingIndex];
        color = directTex.Load(pixel).rgb;
    }
    if (g_GiDisplayMode != GiDisplaySpecular && (g_GiFlags & GiHasDiffuse) != 0u)
    {
        Texture2D<float4> diffuseTex = ResourceDescriptorHeap[g_DiffuseGiIndex];
        color += diffuseTex.Load(pixel).rgb;
    }
    if (g_GiDisplayMode != GiDisplayDiffuse && (g_GiFlags & GiHasSpecular) != 0u)
    {
        Texture2D<float4> specularTex = ResourceDescriptorHeap[g_SpecularGiIndex];
        color += specularTex.Load(pixel).rgb;
    }

    // 各输入已应用接收面的材质响应。合成只相加，输出到 8 位后缓冲时再截断和抖动。
    color = saturate(color);
    if (g_GiDisplayMode == GiDisplayFull && (g_GiFlags & GiEnableDither) != 0u)
        color += Dither8(uint2(pixel.xy));
    return float4(color, 1.0f);
}
