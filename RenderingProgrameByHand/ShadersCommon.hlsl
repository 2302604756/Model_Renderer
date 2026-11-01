#ifndef SHADERS_COMMON_HLSL
#define SHADERS_COMMON_HLSL

// =============================================================================
// ShadersCommon.hlsl
// 所有 pass 共享的类型 / 资源 / 工具函数
// 只放声明和 inline 函数，不放任何 entry point
// =============================================================================

// ---- b0：每物体常量（40 DWORD = 160 B，对应 C++ PerObjectConstants）----
struct PerObjectData
{
    row_major float4x4 world; // 64（转置存储）
    row_major float4x4 normalMatrix; // 64（world 的逆）
    float4 albedo; // 16
    uint vbIndex; // 4
    uint textureIndex; // 4
    uint2 padding; // 8
};

// ---- b1：每帧常量（对应 C++ PerFrameConstants）----
struct PerFrameData
{
    row_major float4x4 viewProj; // 64
    row_major float4x4 invViewProj; // 64
    float4 cameraPos; // 16
    float time; // 4
    uint frameIndex; // 4
    float2 padding; // 8
};

// ---- 顶点格式（48 B，对应 C++ Vertex）----
struct Vertex
{
    float3 position; // 12
    float3 normal; // 12
    float2 uv; // 8
    float4 tangent; // 16
};

// ---- 全局资源 ----
ConstantBuffer<PerObjectData> g_Object : register(b0);
ConstantBuffer<PerFrameData> g_Frame : register(b1);
sampler g_LinearSampler : register(s0);

// ---- b3：太阳 / 天空 / 环境 ----
cbuffer LightingCB : register(b3)
{
    float4 sunDirection; // xyz 光传播方向, w 强度
    float4 sunColor;
    float4 skyColor;
    float4 ambient;
};

float3 SunL()
{
    return -normalize(sunDirection.xyz); // 表面指向光源
}

// ---- t3：场景 TLAS ----
RaytracingAccelerationStructure g_TLAS : register(t3);

// ---- bindless：每实例 GPU 数据 ----
struct GPUObjectData
{
    row_major float4x4 world;
    row_major float4x4 normalMatrix;
    float4 albedo;
    uint vbIndex;
    uint ibIndex;
    uint indexCount;
    uint padding;
};

// ---- 随机 / 低差异采样 ----
float Hash01(uint2 p, uint seed)
{
    uint h = p.x * 1973u + p.y * 9277u + seed * 26699u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return float(h & 0x00FFFFFFu) / 16777216.0f;
}

// R2 低差异序列（两维、均匀、无表）
float2 R2Sequence(uint index)
{
    const float2 g = float2(0.754877666f, 0.569840291f); // 1/φ2, 1/φ3
    return frac(float2(0.5f, 0.5f) + float(index) * g);
}

// 每像素固定旋转（Cranley-Patterson）
float2 PerPixelRotation(uint2 pixel)
{
    return float2(Hash01(pixel, 0u), Hash01(pixel, 1u));
}

// 全局采样索引 = 帧号 × 每帧样本数 + 样本序号
float2 QuasiRandom2D(uint2 pixel, uint sampleIndex)
{
    return frac(R2Sequence(sampleIndex) + PerPixelRotation(pixel));
}

// 余弦加权半球采样
float3 SampleCosineHemisphere(float3 N, float u1, float u2)
{
    float r = sqrt(u1);
    float phi = 6.2831853f * u2;
    float3 t = (abs(N.y) < 0.999f) ? normalize(cross(N, float3(0.0f, 1.0f, 0.0f)))
                                   : float3(1.0f, 0.0f, 0.0f);
    float3 b = cross(N, t);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + N * sqrt(max(0.0f, 1.0f - u1)));
}

//=============================================================================
// 斐波那契球面采样（论文的 stochastically-rotated Fibonacci spiral）
// i: 第几个点  n: 总点数  rot: [0,1)^2 随机旋转
#define GOLDEN_ANGLE 2.39996323f   // 2π/φ² ≈ 137.5°
float3 FibonacciSphere(uint i, uint n, float2 rot)
{
    // z 在 [-1, 1] 均匀分层
    float z = 1.0f - 2.0f * ((float(i) + 0.5f) / float(n));

    // 方位角 = 黄金角前进 + 随机相位
    float phi = float(i) * GOLDEN_ANGLE + rot.x * 6.2831853f;

    float r = sqrt(max(0.0f, 1.0f - z * z));
    return float3(r * cos(phi), z, r * sin(phi));
}

// 绕 Y 轴旋转
float3 RotateY(float3 v, float a)
{
    float s, c;
    sincos(a, s, c);
    return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

// 绕 X 轴旋转
float3 RotateX(float3 v, float a)
{
    float s, c;
    sincos(a, s, c);
    return float3(v.x, c * v.y - s * v.z, s * v.y + c * v.z);
}

// 随机旋转整个螺旋：rot 的两个分量分别控制绕 Y / X 的旋转角
float3 StochasticFibonacci(uint i, uint n, float2 rot)
{
    float3 d = FibonacciSphere(i, n, float2(0.0f, 0.0f)); // 固定螺旋
    d = RotateY(d, rot.x * 6.2831853f);
    d = RotateX(d, rot.y * 6.2831853f);
    return d;
}

//DDGI的球面到正方形映射
// 方向 → 八面体 UV [0,1]^2
float2 OctEncode(float3 n)
{
    //投影到八面体上
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 uv = n.xy;
    if (n.z < 0.0f)
    {
        uv = (1.0f - abs(uv.yx)) * float2(
            uv.x >= 0.0f ? 1.0f : -1.0f,
            uv.y >= 0.0f ? 1.0f : -1.0f);
    }
    return uv * 0.5f + 0.5f;
}

// 八面体 UV [0,1]^2 → 方向
float3 OctDecode(float2 uv)
{
    uv = uv * 2.0f - 1.0f;
    float3 n = float3(uv, 1.0f - abs(uv.x) - abs(uv.y));
    if (n.z < 0.0f)
    {
        n.xy = (1.0f - abs(n.yx)) * float2(
            n.x >= 0.0f ? 1.0f : -1.0f,
            n.y >= 0.0f ? 1.0f : -1.0f);
    }
    return normalize(n);
}


// ---- 天空色（GI 未命中时使用）----
float3 SkyColor()
{
    return skyColor.rgb;
}

// ---- 光追硬阴影 ----
float TraceSunShadow(float3 worldPos, float3 N, float3 L)
{
    RayDesc ray;
    ray.Origin = worldPos + N * 0.002f; // 防自遮挡
    ray.Direction = L;
    ray.TMin = 0.0f;
    ray.TMax = 100.0f;

    RayQuery < RAY_FLAG_NONE > query;
    query.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0xFF, ray);
    query.Proceed();

    return (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0f : 1.0f;
}

//用于Compute Shader求深度矩
// 沿射线求命中距离；未命中返回 tMax（用于 Chebyshev 的"很远"）
float TraceHitDistance(float3 origin, float3 dir, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.0f;
    ray.TMax = tMax;

    RayQuery < RAY_FLAG_NONE > query;
    query.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0xFF, ray);
    query.Proceed();

    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        return query.CommittedRayT();

    return tMax;
}


// ---- b2：Lighting pass 参数（12 DWORD = 48 B）----
struct LightingParams
{
    uint albedoIndex;
    uint normalIndex;
    uint depthIndex;
    uint debugMode;
    uint objectIndex; // GPUObjectData buffer 的 bindless 槽
    uint lightCount;
    uint computeTextureIndex;
    uint distanceAtlasIndex; // 距离矩 atlas SRV
    float4 gridOriginSpacing; // xyz origin, w spacing
    uint4 gridCountsAndProbeIndex; // xyz counts, w probe SRV index
};

ConstantBuffer<LightingParams> g_Lighting : register(b2);

// ---- t5：光源数组 ----
struct GPULight
{
    float4 positionType; // xyz 位置, w 类型（0 平行光 / 1 点光 / 2 聚光）
    float4 directionRange; // xyz 传播方向, w range
    float4 colorIntensity; // rgb 颜色, w 强度
    float4 coneParams; // x cosOuter, y cosInner
};

StructuredBuffer<GPULight> g_Lights : register(t5);

// ---- 直接光照（多光源）----
float3 EvaluateDirectLighting(float3 P, float3 N)
{
    float3 result = float3(0, 0, 0);

    for (uint i = 0; i < g_Lighting.lightCount; ++i)
    {
        GPULight l = g_Lights[i];
        uint type = (uint) l.positionType.w;

        float3 L;
        float atten = 1.0f;

        if (type == 0)
        {
            // 平行光
            L = -normalize(l.directionRange.xyz);
        }
        else
        {
            float3 d = l.positionType.xyz - P;
            float dist = length(d);
            L = d / max(dist, 1e-4);
            atten = saturate(1.0f - dist / max(l.directionRange.w, 1e-4));
            atten *= atten;

            if (type == 2)
            {
                // 聚光
                float cosA = dot(-L, normalize(l.directionRange.xyz));
                atten *= smoothstep(l.coneParams.x, l.coneParams.y, cosA);
            }
        }

        float ndotl = saturate(dot(N, L));
        if (ndotl <= 0.0f)
            continue;

        float shadow = (type == 0) ? TraceSunShadow(P, N, L) : 1.0f;
        result += l.colorIntensity.rgb * (l.colorIntensity.w * ndotl * atten * shadow);
    }

    return result;
}

// 沿一条射线求辐射亮度：命中点着色 + 天空
float3 TraceRadiance(float3 origin, float3 dir)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.0f;
    ray.TMax = 100.0f;

    RayQuery < RAY_FLAG_NONE > query;
    query.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0xFF, ray);
    query.Proceed();

    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return SkyColor();

    StructuredBuffer<GPUObjectData> objects = ResourceDescriptorHeap[g_Lighting.objectIndex];
    GPUObjectData obj = objects[query.CommittedInstanceID()];

    float2 bary = query.CommittedTriangleBarycentrics();
    uint prim = query.CommittedPrimitiveIndex();

    StructuredBuffer<uint> ib = ResourceDescriptorHeap[obj.ibIndex];
    uint i0 = ib[prim * 3 + 0];
    uint i1 = ib[prim * 3 + 1];
    uint i2 = ib[prim * 3 + 2];

    StructuredBuffer<Vertex> vb = ResourceDescriptorHeap[obj.vbIndex];
    Vertex v0 = vb[i0];
    Vertex v1 = vb[i1];
    Vertex v2 = vb[i2];

    float w0 = 1.0f - bary.x - bary.y;
    float3 localPos = v0.position * w0 + v1.position * bary.x + v2.position * bary.y;
    float3 localNrm = v0.normal * w0 + v1.normal * bary.x + v2.normal * bary.y;

    float3 hitPos = mul(obj.world, float4(localPos, 1.0f)).xyz;
    float3 hitN = normalize(mul(obj.normalMatrix, float4(localNrm, 0.0f)).xyz);

    if (dot(hitN, dir) > 0.0f)
        hitN = -hitN;

    return obj.albedo.rgb * (EvaluateDirectLighting(hitPos, hitN) + SkyColor() * 0.3f);
}

#endif // SHADERS_COMMON_HLSL