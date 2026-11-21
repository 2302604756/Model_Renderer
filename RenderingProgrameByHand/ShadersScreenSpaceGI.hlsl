#include "ShadersCommon.hlsl"
#include "GiPassBindings.hlsli"

struct ScreenGiVertex
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

struct ScreenGiSurface
{
    uint2 pixel;
    uint2 size;
    float3 position;
    float3 normal;
};

bool LoadScreenGiSurface(Texture2D<float> depthTex, Texture2D<float4> normalTex,
    ScreenGiVertex input, out ScreenGiSurface surface)
{
    surface = (ScreenGiSurface) 0;
    surface.pixel = uint2(input.position.xy);
    depthTex.GetDimensions(surface.size.x, surface.size.y);
    float depth = depthTex.Load(int3(surface.pixel, 0));
    if (depth >= 1.0f)
        return false;

    float2 ndc = input.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    float4 world = mul(g_Frame.invViewProj, float4(ndc, depth, 1.0f));
    float3 normal = normalTex.Load(int3(surface.pixel, 0)).xyz;
    if (abs(world.w) < 1e-6f || dot(normal, normal) < 1e-6f)
        return false;
    surface.position = world.xyz / world.w;
    surface.normal = normalize(normal);
    return true;
}

bool ReadScreenDepth(Texture2D<float> depthTex, uint2 size, float4 clip,
    out uint2 pixel, out float gap)
{
    pixel = 0;
    gap = 0.0f;
    if (clip.w <= 0.0f || clip.z < 0.0f || clip.z > clip.w)
        return false;

    float2 uv = clip.xy / clip.w * float2(0.5f, -0.5f) + 0.5f;
    if (any(uv < 0.0f) || any(uv >= 1.0f))
        return false;

    pixel = uint2(uv * float2(size));
    float depth = depthTex.Load(int3(pixel, 0));
    if (depth >= 1.0f)
    {
        gap = -1.0f;
        return true;
    }

    // 在当前透视投影中，反投影的齐次 w 只取决于深度。其倒数是视空间 z。
    float inverseViewZ = dot(g_Frame.invViewProj[3].zw, float2(depth, 1.0f));
    gap = clip.w - rcp(max(inverseViewZ, 1e-6f));
    return true;
}

bool TraceScreenGiRay(Texture2D<float> depthTex, Texture2D<float4> normalTex,
    ScreenGiSurface receiver, float3 rayDirection, out uint2 hitPixel)
{
    hitPixel = 0;
    float3 origin = receiver.position + 0.025f * receiver.normal;
    float4 clipOrigin = mul(g_Frame.viewProj, float4(origin, 1.0f));
    float4 clipDirection = mul(g_Frame.viewProj, float4(rayDirection, 0.0f));
    float t = 0.075f;
    float stride = 0.10f;
    float previousT = 0.0f;
    bool previousInFront = true;

    // 在世界空间推进，在屏幕上比较射线与当前可见深度。
    // 远处渐进放宽步距，离开视锥或超过步数时返回未命中。
    [loop]
    for (uint step = 0; step < 18; ++step)
    {
        uint2 candidate;
        float gap;
        float4 clip = clipOrigin + t * clipDirection;
        if (!ReadScreenDepth(depthTex, receiver.size, clip, candidate, gap))
            return false;

        if (gap > 0.0f && previousInFront)
        {
            // 找到深度穿越后细化交点，避免短程大步长直接越过薄表面。
            float lower = previousT;
            float upper = t;
            [unroll]
            for (uint refine = 0; refine < 3; ++refine)
            {
                float middle = 0.5f * (lower + upper);
                uint2 middlePixel;
                float middleGap;
                if (ReadScreenDepth(depthTex, receiver.size,
                    clipOrigin + middle * clipDirection, middlePixel, middleGap) && middleGap > 0.0f)
                {
                    upper = middle;
                    candidate = middlePixel;
                    gap = middleGap;
                }
                else
                    lower = middle;
            }

            // 视空间厚度限制、像素间距和命中法线共同排除自交及明显的轮廓误命中。
            float thickness = clamp(0.025f * (clipOrigin.w + upper * clipDirection.w), 0.045f, 0.15f);
            int2 separation = abs(int2(candidate) - int2(receiver.pixel));
            float3 hitNormal = normalTex.Load(int3(candidate, 0)).xyz;
            if (gap <= thickness && max(separation.x, separation.y) > 1 &&
                dot(hitNormal, -rayDirection) > 0.03f)
            {
                hitPixel = candidate;
                return true;
            }
        }

        previousInFront = gap <= 0.0f;
        previousT = t;
        t += stride;
        stride *= 1.15f;
        if (t > 6.0f)
            break;
    }
    return false;
}

float4 ScreenSpaceDiffusePS(ScreenGiVertex input) : SV_Target
{
    Texture2D<float> depthTex = ResourceDescriptorHeap[g_Lighting.depthIndex];
    Texture2D<float4> normalTex = ResourceDescriptorHeap[g_Lighting.normalIndex];
    Texture2D<float4> albedoTex = ResourceDescriptorHeap[g_Lighting.albedoIndex];
    Texture2D<float4> directTex = ResourceDescriptorHeap[g_DirectLightingIndex];
    ScreenGiSurface receiver;
    if (!LoadScreenGiSurface(depthTex, normalTex, input, receiver))
        return 0.0f;

    // 固定屏幕样本，余弦半球均值给出乘接收反照率之前的散射量。
    const uint sampleCount = 3;
    float3 radiance = 0.0f;
    [loop]
    for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
    {
        float2 xi = QuasiRandom2D(receiver.pixel, sampleIndex + 1u);
        float3 direction = SampleCosineHemisphere(receiver.normal, xi.x, xi.y);
        uint2 hitPixel;
        if (TraceScreenGiRay(depthTex, normalTex, receiver, direction, hitPixel))
            radiance += directTex.Load(int3(hitPixel, 0)).rgb;
    }

    float3 albedo = albedoTex.Load(int3(receiver.pixel, 0)).rgb;
    return float4(albedo * radiance / float(sampleCount), 0.0f);
}

float3 SampleScreenGiGgxHalfVector(float3 normal, float alpha, float2 xi)
{
    float phi = 6.28318530718f * xi.x;
    float cosTheta = sqrt((1.0f - xi.y) / (1.0f + (alpha * alpha - 1.0f) * xi.y));
    float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    float3 helper = abs(normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(helper, normal));
    float3 bitangent = cross(normal, tangent);
    return tangent * (cos(phi) * sinTheta) + bitangent * (sin(phi) * sinTheta) + normal * cosTheta;
}

float ScreenGiSmithMask(float cosine, float alpha)
{
    float a2 = alpha * alpha;
    return 2.0f * cosine / (cosine + sqrt(a2 + (1.0f - a2) * cosine * cosine));
}

float4 ScreenSpaceSpecularPS(ScreenGiVertex input) : SV_Target
{
    Texture2D<float> depthTex = ResourceDescriptorHeap[g_Lighting.depthIndex];
    Texture2D<float4> normalTex = ResourceDescriptorHeap[g_Lighting.normalIndex];
    Texture2D<float4> directTex = ResourceDescriptorHeap[g_DirectLightingIndex];
    ScreenGiSurface receiver;
    if (!LoadScreenGiSurface(depthTex, normalTex, input, receiver))
        return 0.0f;

    float3 view = normalize(g_Frame.cameraPos.xyz - receiver.position);
    float3 normal = receiver.normal;
    float viewCosine = dot(view, normal);
    if (viewCosine <= 0.001f)
        return 0.0f;

    // 当前 G-buffer 尚无材质粗糙度和金属度。示例统一采用 F0=0.04 的电介质和 0.22 粗糙度。
    const float roughness = 0.22f;
    const float alpha = roughness * roughness;
    const float f0 = 0.04f;
    const uint sampleCount = 2;
    float3 radiance = 0.0f;
    [loop]
    for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
    {
        float2 xi = QuasiRandom2D(receiver.pixel, sampleIndex + 11u);
        float3 halfVector = SampleScreenGiGgxHalfVector(normal, alpha, xi);
        float viewHalfCosine = dot(view, halfVector);
        float3 direction = reflect(-view, halfVector);
        float rayCosine = dot(direction, normal);
        if (viewHalfCosine <= 0.0f || rayCosine <= 0.0f)
            continue;

        uint2 hitPixel;
        if (!TraceScreenGiRay(depthTex, normalTex, receiver, direction, hitPixel))
            continue;

        float3 source = directTex.Load(int3(hitPixel, 0)).rgb;
        if ((g_GiFlags & GiHasDiffuse) != 0u)
        {
            Texture2D<float4> diffuseTex = ResourceDescriptorHeap[g_DiffuseGiIndex];
            source += diffuseTex.Load(int3(hitPixel, 0)).rgb;
        }

        // 按 GGX 半角向量采样时，微表面分布项与采样 PDF 抵消。
        float fresnel = f0 + (1.0f - f0) * pow(1.0f - saturate(viewHalfCosine), 5.0f);
        float visibility = ScreenGiSmithMask(viewCosine, alpha) * ScreenGiSmithMask(rayCosine, alpha);
        float halfCosine = max(0.001f, dot(normal, halfVector));
        radiance += source * (fresnel * visibility * viewHalfCosine / (viewCosine * halfCosine));
    }
    return float4(radiance / float(sampleCount), 0.0f);
}
