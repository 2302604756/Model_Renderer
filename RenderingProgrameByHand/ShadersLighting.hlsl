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

    return atlas.SampleLevel(g_LinearSampler, atlasUV, 0.0f).rgb;
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

    return distAtlas.SampleLevel(g_LinearSampler, atlasUV, 0.0f).xyz;
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

// 8 探针 cage + 三线性 + 背面权重 + Chebyshev 可见性
DDGISample SampleDDGI(float3 P, float3 N, uint2 pixel, uint pointMask)
{
    StructuredBuffer<uint> probeMasks = ResourceDescriptorHeap[probeMaskIndex];
    // 探针 relocation 的每探针世界空间偏移（与探针射线起点用同一份数据）
    RWStructuredBuffer<float4> probeData = ResourceDescriptorHeap[probeDataUavIndex];
    
    uint3 counts = g_Lighting.gridCountsAndProbeIndex.xyz;
    float3 origin = g_Lighting.gridOriginSpacing.xyz;
    float spacing = g_Lighting.gridOriginSpacing.w;

    // 调试累计
    float validCount = 0.0f;
    float chebWeighted = 0.0f;
    float dominantW = -1.0f;
    uint dominantProbe = 0;

    DDGISample result;
    result.E = float3(0.0f, 0.0f, 0.0f);
    result.wsum = 0.0f;
    result.validCount = 0.0f;
    result.chebAvg = 0.0f;
    result.dominantProbe = 0;

    // 网格坐标
    float3 gridPos = (P - origin) / spacing;
    float3 base = floor(gridPos);
    float3 f = gridPos - base;
    
    // 可见性测试点的偏置：法线 + 3 倍视线方向
    // 作用是把测试点推离明暗边界，避免在表面自遮挡
    float normalBias = spacing * 0.15f;
    float3 wo = normalize(g_Frame.cameraPos.xyz - P);
    float3 visibilityBias = (N + 3.0f * wo) * normalBias;

    float3 irradiance = float3(0.0f, 0.0f, 0.0f);
    float wsum = 0.0f;


    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        // 8 个角：bit0=x，bit1=y，bit2=z
        //根据i得到八个角点的索引
        int3 corner = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        int3 idx = clamp(int3(base) + corner,
                         int3(0, 0, 0), int3(counts) - 1);

        //一维索引
        uint probeIndex = (idx.z * counts.y + idx.y) * counts.x + idx.x;

        //计算Mask
        uint probeMask = probeMasks[probeIndex];
        bool maskMatch = (probeMask & pointMask) != 0u;
        
        // 三线性权重单独保存，最后再乘（官方顺序）
        float3 w3 = lerp(1.0f - f, f, float3(corner));
        float trilinearW = w3.x * w3.y * w3.z;

          // 探针位置 = 格点 + relocation 偏移；三线性权重仍按格点算（与 RTXGI 一致）
        float3 probePos = origin + float3(idx) * spacing + probeData[probeIndex].xyz;

        // 可见性查询用偏置后的点
        float3 probeToPoint = P - probePos + visibilityBias;
        float dist = length(probeToPoint) + 1e-4f;
        float3 dirToProbe = -probeToPoint / dist;

        // 背面测试用未偏置方向（补充代码里也是这么做的）
        float3 trueDirToProbe = normalize(probePos - P);


        // 探针质量权重从 1 开始，不包含三线性
        float weight = 1.0f;

        // 1. wrap shading  背面剔除
        float wrap = saturate((dot(trueDirToProbe, N) + 1.0f) * 0.5f);
        weight *= (wrap * wrap) + 0.2f;

        // 2. 有效性（软权重）
        float3 moments = SampleProbeDistance(probeIndex, -dirToProbe);
        weight *= moments.z;
        
        // 调试：有效探针计数
        if (moments.z > 0.5f)
            validCount += 1.0f;

        float rawMean = moments.x;
        float rawMean2 = moments.y;

        // VSM 里 mean² - mean2 可能为负，用 abs；再加 variance bias 平滑
        const float varianceBias = 0.02f;
        float variance = abs(rawMean * rawMean - rawMean2) + varianceBias;

        // Chebyshev bias：稍微抬高均值，减少薄墙/接触处的误遮挡
        const float chebyshevBias = 0.07f;
        float mean = max(rawMean + chebyshevBias, 0.0f);

        float cheb = 1.0f;
        if (dist > mean)
        {
            float d = dist - mean;                  //这样写的原因是为了让物体在距离探针mean左右能连续
            cheb = variance / (variance + d * d);
        }

        // 提高对比度：三次方；不要 0.05 下限
        // 3.切比雪夫
        cheb = max(cheb * cheb * cheb, 0.05f);
       // weight *= cheb;


        // 4. 避免为零
        weight = max(1e-6f, weight);
       
        // 5. crush 小权重（只作用在质量权重上）
        const float crushThreshold = 0.05f;
        if (weight < crushThreshold)
            weight *= (weight * weight) * (1.0f / (crushThreshold * crushThreshold));

        // 不同 volume 的 probe 不参与插值
        if (!maskMatch)
            weight = 0.0f;
        
        // 6. 最后乘三线性权重
        weight *= trilinearW;
        
        // 必须放在三线性之后，这样 chebAvg 反映的是最终权重下的平均可见性
        chebWeighted += cheb * weight;
        
        // debug 15：只保留三线性权重，验证 E 本身是否连续
        if (g_Lighting.debugMode == 15)
        {
            weight = trilinearW;
        }
        
        
        // 调试：记录主导探针和加权 Chebyshev
        if (weight > dominantW)
        {
            dominantW = weight;
            dominantProbe = probeIndex;
        }


        // 测试bug是否与采样权重有关
        //weight = 1.0f;
        
        // 按法线方向查该探针的 irradiance
        float3 E = SampleProbeIrradiance(probeIndex, N);

        irradiance += E * weight;
        wsum += weight;
    }

    
    // 线性加权平均
    // cage 补位（两物体过近导致 cage 探针全失效时的兜底）
    //   触发条件：8 个角的有效权重和 wsum < 0.02（正常场景 wsum 中位数约 0.56，不会触发）
    //   A) 先用 fixup 补过的数据重算 8 个角：validity 视作 1、关掉 Chebyshev（距离矩不可信）
    //   B) 8 个角也被 mask 全剔掉时，扩到 ±1 格子的 27 邻域，用反距离加权（保留 wrap/validity/Chebyshev）
    if (wsum < 0.02f)
    {
        float3 accA = float3(0.0f, 0.0f, 0.0f);
        float accWA = 0.0f;
        [unroll]
        for (uint i2 = 0; i2 < 8; ++i2)
        {
            int3 corner2 = int3(i2 & 1, (i2 >> 1) & 1, (i2 >> 2) & 1);
            int3 idx2 = clamp(int3(base) + corner2, int3(0, 0, 0), int3(counts) - 1);
            uint p2 = (idx2.z * counts.y + idx2.y) * counts.x + idx2.x;
            if ((probeMasks[p2] & pointMask) == 0u)
                continue;

            // 三线性权重与主循环一致，保证空间连续
            float3 w3b = lerp(1.0f - f, f, float3(corner2));
            float tw = w3b.x * w3b.y * w3b.z;

            float3 p2Pos = origin + float3(idx2) * spacing + probeData[p2].xyz;
            float3 trueDir2 = normalize(p2Pos - P);
            float wrap2 = saturate((dot(trueDir2, N) + 1.0f) * 0.5f);

            float w2 = ((wrap2 * wrap2) + 0.2f) * tw;
            accA += SampleProbeIrradiance(p2, N) * w2;
            accWA += w2;
        }
        if (accWA > 1e-5f)
        {
            result.E = accA / accWA;
            result.wsum = 1.0f;
            result.validCount = 8.0f;
            result.chebAvg = 1.0f;
            result.dominantProbe = 0;
            return result;
        }

        // B) 27 邻域兜底
        float3 acc = float3(0.0f, 0.0f, 0.0f);
        float accW = 0.0f;
        int3 baseIdx = int3(floor(gridPos)) - 1;
        [unroll]
        for (int k = 0; k < 27; ++k)
        {
            int3 off = int3(k % 3, (k / 3) % 3, k / 9);
            int3 i2 = clamp(baseIdx + off, int3(0, 0, 0), int3(counts) - 1);
            uint p2 = (i2.z * counts.y + i2.y) * counts.x + i2.x;
            if ((probeMasks[p2] & pointMask) == 0u)
                continue;

            float3 p2Pos = origin + float3(i2) * spacing + probeData[p2].xyz;
            float3 d2v = P - p2Pos;
            float dist2 = max(length(d2v), 1e-3f);
            float3 dirTo2 = -d2v / dist2;

            float3 m2 = SampleProbeDistance(p2, dirTo2);

            float3 trueDir2 = normalize(p2Pos - P);
            float wrap2 = saturate((dot(trueDir2, N) + 1.0f) * 0.5f);
            float w2 = (wrap2 * wrap2) + 0.2f;
            w2 *= m2.z;

            float var2 = abs(m2.x * m2.x - m2.y) + 0.02f;
            float mean2 = max(m2.x + 0.07f, 0.0f);
            float cheb2 = 1.0f;
            if (dist2 > mean2)
            {
                float dd = dist2 - mean2;
                cheb2 = var2 / (var2 + dd * dd);
            }
            cheb2 = max(cheb2 * cheb2 * cheb2, 0.05f);
            w2 *= cheb2;
            w2 *= 1.0f / (1.0f + dist2 * dist2 * 4.0f);

            acc += SampleProbeIrradiance(p2, N) * w2;
            accW += w2;
        }
        if (accW > 1e-5f)
        {
            result.E = acc / accW;
            result.wsum = 1.0f;
            result.validCount = 8.0f;
            result.chebAvg = 1.0f;
            result.dominantProbe = 0;
            return result;
        }
    }
    
    // 8 个 cage probe 全被筛掉/全无效时，退回环境光，避免纯黑阶跃
    if (wsum < 1e-4f)
        result.E = ambient.rgb;
    else
        result.E = irradiance / max(wsum, 1e-5f);


    result.wsum = wsum;
    result.validCount = validCount;
    result.chebAvg = chebWeighted / max(wsum, 1e-5f);
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

    
    // debug 8：DDGI 8 探针插值结果（除以 π 便于和直射比较亮度）
    if (g_Lighting.debugMode == 8)
    {
        DDGISample s = SampleDDGI(world.xyz, N, pixel, pointMask);
        return float4(s.E, 1.0f);
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

        // 采样一次，debug 11~14 和 final 共用
DDGISample ddgi = SampleDDGI(world.xyz, N, pixel, pointMask);

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
    
    if (g_Lighting.debugMode == 15)
    {
        DDGISample s = SampleDDGI(world.xyz, N, pixel, pointMask);
        return float4(s.E, 1.0f);
    }
    
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