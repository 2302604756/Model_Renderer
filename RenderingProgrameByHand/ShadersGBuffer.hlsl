#include "ShadersCommon.hlsl"

// =============================================================================
// G-Buffer pass：VSMain + GBufferPS
// =============================================================================

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
    float4 normal : NORMAL;
};

PSInput VSMain(uint vertexID : SV_VertexID)
{
    PSInput vout;

    // 从 bindless 堆获取 VB 的 StructuredBuffer 视图（Vertex Pulling）
    StructuredBuffer<Vertex> vertexBuffer = ResourceDescriptorHeap[g_Object.vbIndex];
    Vertex vin = vertexBuffer[vertexID];

    // 局部空间 -> 世界空间 -> 裁剪空间
    float4 worldPos = mul(g_Object.world, float4(vin.position, 1.0f));
    vout.position = mul(g_Frame.viewProj, worldPos);

    vout.uv = vin.uv;

    // 法线：方向向量（w=0），用法线矩阵变换到世界空间
    vout.normal = mul(g_Object.normalMatrix, float4(vin.normal, 0.0f));
    return vout;
}

// G-buffer 输出：两个 RT
struct GBufferOutput
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    uint volumeMask : SV_Target2; // R32_UINT
};

GBufferOutput GBufferPS(PSInput pin)
{
    GBufferOutput o;
    o.albedo = g_Object.albedo;
    o.normal = float4(normalize(pin.normal.xyz), 1.0f);
    o.volumeMask = g_Object.volumeMask;
    return o;
}