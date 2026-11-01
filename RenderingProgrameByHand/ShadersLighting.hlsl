#include "ShadersCommon.hlsl"

// =============================================================================
// Lighting pass：TraceGI + 全屏三角形 + 最终着色
// =============================================================================

// 一次弹射的间接光：u 是算好的低差异 2D 样本
float3 TraceGI(float3 worldPos, float3 N, float2 u)
{
    float3 dir = SampleCosineHemisphere(N, u.x, u.y);
    return TraceRadiance(worldPos + N * 0.01f, dir);
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

    return atlas.SampleLevel(g_LinearSampler, atlasUV, 0.0f).rgb;
}

// 采样单个探针的距离矩（x = E[d]，y = E[d²]）
float2 SampleProbeDistance(uint probeIndex, float3 dir)
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

    return distAtlas.SampleLevel(g_LinearSampler, atlasUV, 0.0f).xy;
}

// 8 探针 cage + 三线性 + 背面权重 + Chebyshev 可见性
float3 SampleDDGI(float3 P, float3 N)
{
    uint3 counts = g_Lighting.gridCountsAndProbeIndex.xyz;
    float3 origin = g_Lighting.gridOriginSpacing.xyz;
    float spacing = g_Lighting.gridOriginSpacing.w;

    // 法线偏移，避开表面处的可见性边界
    float3 Pn = P + N * 0.02f;

    // 网格坐标
    float3 gridPos = (Pn - origin) / spacing;
    float3 base = floor(gridPos);
    float3 f = gridPos - base;

    float3 irradiance = float3(0.0f, 0.0f, 0.0f);
    float wsum = 0.0f;

    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        // 8 个角：bit0=x，bit1=y，bit2=z
        int3 corner = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        int3 idx = clamp(int3(base) + corner,
                         int3(0, 0, 0), int3(counts) - 1);

        uint probeIndex = (idx.z * counts.y + idx.y) * counts.x + idx.x;

        // 三线性权重
        float3 w3 = lerp(1.0f - f, f, float3(corner));
        float w = w3.x * w3.y * w3.z;

        // 探针位置与方向
        float3 probePos = origin + float3(idx) * spacing;
        float3 toProbe = probePos - Pn;
        float dist = length(toProbe) + 1e-4f;
        float3 dirToProbe = toProbe / dist;

        // 背面软剔除：探针在切平面以下时平滑降权
        float backface = saturate((dot(N, dirToProbe) + 0.2f) / 1.2f);
        backface = backface * backface;
        backface = backface * backface;
        w *= backface;

        // Chebyshev 可见性
        float2 moments = SampleProbeDistance(probeIndex, -dirToProbe);
        float mean = moments.x;
        float variance = max(moments.y - mean * mean, 1e-4f);

        float cheb = 1.0f;
        if (dist > mean)
        {
            float d = dist - mean;
            cheb = variance / (variance + d * d);
        }
        w *= cheb;

        if (w <= 1e-5f)
            continue;

        // 按法线方向查该探针的 irradiance
        float3 E = SampleProbeIrradiance(probeIndex, N);

        irradiance += E * w;
        wsum += w;
    }

    return irradiance / max(wsum, 1e-4f);
}


float4 LightingPS(LightingVSOut pin) : SV_Target
{
    Texture2D<float4> albedoTex = ResourceDescriptorHeap[g_Lighting.albedoIndex];
    Texture2D<float4> normalTex = ResourceDescriptorHeap[g_Lighting.normalIndex];
    Texture2D<float> depthTex = ResourceDescriptorHeap[g_Lighting.depthIndex];

    float4 albedo = albedoTex.SampleLevel(g_LinearSampler, pin.uv, 0.0f);
    float3 N = normalize(normalTex.SampleLevel(g_LinearSampler, pin.uv, 0.0f).xyz);

    uint2 pixel = uint2(pin.position.xy);
    float depth = depthTex.Load(int3(pixel, 0));

    // ---- debug 视图 ----
    if (g_Lighting.debugMode == 1)
        return albedo;
    if (g_Lighting.debugMode == 2)
        return float4(N * 0.5f + 0.5f, 1.0f);
    if (g_Lighting.debugMode == 3)
        return float4(depth, depth, depth, 1.0f);

    // ---- depth -> 世界坐标 ----
    float2 ndc = float2(pin.uv.x * 2.0f - 1.0f, 1.0f - pin.uv.y * 2.0f);
    float4 world = mul(g_Frame.invViewProj, float4(ndc, depth, 1.0f));
    world.xyz /= world.w;

    if (g_Lighting.debugMode == 4)
        return float4(world.xyz * 0.25f + 0.5f, 1.0f);

    // debug 5：光追阴影 mask
    if (g_Lighting.debugMode == 5)
    {
        float s = TraceSunShadow(world.xyz, N, SunL());
        return float4(s, s, s, 1.0f);
    }
    
        // debug 7：compute 输出
    if (g_Lighting.debugMode == 7)
    {
        Texture2D<float4> computeTex = ResourceDescriptorHeap[g_Lighting.computeTextureIndex];
        return computeTex.SampleLevel(g_LinearSampler, pin.uv, 0.0f);
    }
    
    // debug 8：DDGI 8 探针插值结果（除以 π 便于和直射比较亮度）
    if (g_Lighting.debugMode == 8)
    {
        float3 E = SampleDDGI(world.xyz, N);
        return float4(E / 3.14159265f, 1.0f);
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
        float2 m = distAtlas.Load(int3(p, 0)).xy;

        // 距离归一化后分别映射到 R/G
        return float4(m.x / 10.0f, saturate(m.y / 100.0f), 0.0f, 1.0f);
    }

    // ---- 纯光追 GI：每像素 2 条余弦半球射线 ----
    const uint giSamples = 2;
    float3 indirect = float3(0.0f, 0.0f, 0.0f);

    for (uint i = 0; i < giSamples; ++i)
    {
        uint sampleIndex = g_Frame.frameIndex * giSamples + i;
        float2 u = QuasiRandom2D(pixel, sampleIndex);
        indirect += TraceGI(world.xyz, N, u);
    }
    indirect /= giSamples;

    // debug 6：只看 GI
    if (g_Lighting.debugMode == 6)
        return float4(albedo.rgb * indirect, 1.0f);

    // ---- final：直射 * 阴影 + 环境 + 间接 ----
    float3 direct = EvaluateDirectLighting(world.xyz, N);
    float3 color = albedo.rgb * (direct + ambient.rgb + indirect);
    return float4(color, 1.0f);
}