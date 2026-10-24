// =============================================================================
// b0: 每物体常量（32-bit root constants，36 DWORD = 144 字节）
// b1: 每帧常量（Root CBV）
// =============================================================================

// 每物体一份：世界矩阵 + 法线矩阵 + bindless 索引
struct PerObjectData
{
    row_major float4x4 world; // 64
    row_major float4x4 normalMatrix; // 64
    float4             albedo; // 16
    uint               vbIndex; // 4
    uint               textureIndex; // 4
    uint2              padding; // 8
};

// 每帧一份：view/proj + 相机 + 时间
struct PerFrameData
{
    row_major float4x4 viewProj; // 64
    row_major float4x4 invViewProj; // 64
    float4 cameraPos; // 16
    float time; // 4
    float3 padding; // 12
};

struct Vertex
{
    float3 position; // 12 Bytes (offset 0)
    float3 normal; // 12 Bytes (offset 12)
    float2 uv; // 8 Bytes  (offset 24)
    float4 tangent; // 16 Bytes (offset 32)
};

ConstantBuffer<PerObjectData> g_Object : register(b0);
ConstantBuffer<PerFrameData> g_Frame : register(b1);
sampler g_LinearSampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
    float4 normal : NORMAL;
};

PSInput VSMain(uint vertexID : SV_VertexID)
{
    PSInput vout;
    //从全局 Bindless 堆获取 VB 的 StructuredBuffer 视图
    StructuredBuffer<Vertex> vertexBuffer = ResourceDescriptorHeap[g_Object.vbIndex];
    Vertex vin = vertexBuffer[vertexID]; //再通过SV_VertexID获取到实际的顶点索引
    
    // 局部空间 -> 世界空间 -> 裁剪空间
    float4 worldPos = mul(g_Object.world, float4(vin.position, 1.0f));
    vout.position = mul(g_Frame.viewProj, worldPos);
    
    vout.uv = vin.uv;
    
    // 法线是方向向量（w = 0），用法线矩阵变换到世界空间
    vout.normal = mul(g_Object.normalMatrix, float4(vin.normal, 0.0f));
    return vout;
}

float4 PSMain(PSInput pin) : SV_Target
{
    Texture2D diffuseTex = ResourceDescriptorHeap[g_Object.textureIndex];

    float3 lightDir = normalize(float3(0.5f, 0.45f, -0.9f)); // 从 z=-2.5 的开口方向照进来
    float3 N = normalize(pin.normal.xyz); // 世界空间法线
    float NdotL = saturate(dot(N, lightDir));
    // float4 baseColor = diffuseTex.Sample(g_LinearSampler, pin.uv);
    float4 baseColor = g_Object.albedo;
    float3 ambient = float3(0.1f, 0.1f, 0.1f);
    float3 finalColor = baseColor.rgb * (NdotL + ambient);
    return float4(finalColor, baseColor.a);
}


// =============================================================================
// 延迟光照 pass
// b2: LightingParams（8 个 32-bit root constants）
// =============================================================================
struct LightingParams
{
    uint albedoIndex;
    uint normalIndex;
    uint depthIndex;
    uint debugMode;
    uint objectIndex; // GPUObjectData buffer 的 bindless 槽（和 C++ 对齐）
    uint3 padding;
};

ConstantBuffer<LightingParams> g_Lighting : register(b2);

// G-buffer 输出：两个 RT
struct GBufferOutput
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1; // 世界空间法线，w 备用
};

GBufferOutput GBufferPS(PSInput pin)
{
    GBufferOutput o;
    o.albedo = g_Object.albedo;
    o.normal = float4(normalize(pin.normal.xyz), 1.0f);
    return o;
}



//=========================================================================================================
//加入光追
RaytracingAccelerationStructure g_TLAS : register(t3);

float TraceSunShadow(float3 worldPos, float3 N, float3 L)
{
    RayDesc ray;
    ray.Origin = worldPos + N * 0.002f;//防止自遮挡
    ray.Direction = L;
    ray.TMin = 0.0f;
    ray.TMax = 100.0f;

    RayQuery < RAY_FLAG_NONE > query;
    query.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0xFF, ray);
    query.Proceed();

    return (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0f : 1.0f;
}


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

// 简单的像素级哈希随机
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

// 天空色（GI 未命中时使用）
float3 SkyColor()
{
    return float3(0.2f, 0.3f, 0.5f);
}

// 一次弹射的间接光（v1：命中取 albedo，未命中取天空）
float3 TraceGI(float3 worldPos, float3 N, float3 L, uint2 pixel, uint seed)
{
    float u1 = Hash01(pixel, seed * 2u + 0u);
    float u2 = Hash01(pixel, seed * 2u + 1u);
    float3 dir = SampleCosineHemisphere(N, u1, u2);                 //通过Hash扰动

    RayDesc ray;
    ray.Origin = worldPos + N * 0.01f;
    ray.Direction = dir;
    ray.TMin = 0.0f;
    ray.TMax = 100.0f;

    RayQuery < RAY_FLAG_NONE > query;
    query.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0xFF, ray);
    query.Proceed();

    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return SkyColor(); // 打到天空

    // ---------- 1. 命中实例数据 ----------
    StructuredBuffer<GPUObjectData> objects = ResourceDescriptorHeap[g_Lighting.objectIndex];
    GPUObjectData obj = objects[query.CommittedInstanceID()];
    
     // ---------- 2. 命中三角形的重心坐标 + 顶点 ----------
    float2 bary = query.CommittedTriangleBarycentrics(); // (b1, b2)，b0 = 1 - b1 - b2
    uint prim = query.CommittedPrimitiveIndex(); // 三角形序号
    
    StructuredBuffer<uint> ib = ResourceDescriptorHeap[obj.ibIndex];
    uint i0 = ib[prim * 3 + 0];
    uint i1 = ib[prim * 3 + 1];
    uint i2 = ib[prim * 3 + 2];
    
    StructuredBuffer<Vertex> vb = ResourceDescriptorHeap[obj.vbIndex];
    Vertex v0 = vb[i0];
    Vertex v1 = vb[i1];
    Vertex v2 = vb[i2];

    // ---------- 3. 插值局部属性，变换到世界 ----------
    float w0 = 1.0f - bary.x - bary.y;
    float3 localPos = v0.position * w0 + v1.position * bary.x + v2.position * bary.y;
    float3 localNrm = v0.normal * w0 + v1.normal * bary.x + v2.normal * bary.y;
    
    float3 hitPos = mul(obj.world, float4(localPos, 1.0f)).xyz;
    float3 hitN = normalize(mul(obj.normalMatrix, float4(localNrm, 0.0f)).xyz);
    
     // 射线可能打到三角形背面：翻转法线，避免阴影射线从背面自交
    if (dot(hitN, dir) > 0.0f)
        hitN = -hitN;
    
     // ---------- 4. 命中点的直接光照 + 天空环境 ----------
    float NdotL = saturate(dot(hitN, L));
    float shadow = TraceSunShadow(hitPos, hitN, L);
    
    // 该点朝观察方向的出射 radiance ≈ albedo * (直射 + 环境)
    return obj.albedo.rgb * (NdotL * shadow + SkyColor() * 0.3f);

}

// 全屏三角形（不用 VB，靠 SV_VertexID 生成 3 个顶点）
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

float4 LightingPS(LightingVSOut pin) : SV_Target
{
    Texture2D<float4> albedoTex = ResourceDescriptorHeap[g_Lighting.albedoIndex];
    Texture2D<float4> normalTex = ResourceDescriptorHeap[g_Lighting.normalIndex];
    Texture2D<float> depthTex = ResourceDescriptorHeap[g_Lighting.depthIndex];
    float3 L = normalize(float3(0.5f, 0.45f, -0.9f));

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
    
    // ---- 方向光（表面指向光源）----
    float NdotL = saturate(dot(N, L));

    // debug 5：光追阴影 mask
    if (g_Lighting.debugMode == 5)
    {
        float s = TraceSunShadow(world.xyz, N, L);
        return float4(s, s, s, 1.0f);
    }

    // ---- 纯光追 GI：每像素 2 条余弦半球射线 ----
    const uint giSamples = 2; 
    uint giSeed = (uint) (g_Frame.time * 60.0f);

    float3 indirect = float3(0.0f, 0.0f, 0.0f);
    for (uint i = 0; i < giSamples; ++i)
        indirect += TraceGI(world.xyz, N, L, pixel, giSeed + i);
    indirect /= giSamples;

    // debug 6：只看 GI（会噪，正常）
    if (g_Lighting.debugMode == 6)
        return float4(albedo.rgb * indirect, 1.0f);

    // ---- final：直射 * 阴影 + 环境 + 间接 ----
    float shadow = TraceSunShadow(world.xyz, N, L);
    float3 color = albedo.rgb * (NdotL * shadow + 0.05f + indirect);
    return float4(color, 1.0f);
}


