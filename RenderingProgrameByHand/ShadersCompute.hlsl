// =============================================================================
// ShadersCompute.hlsl
// 目前：compute 冒烟；以后 DDGI 的 ProbeUpdateCS 也放这里
// =============================================================================

struct ComputeParams
{
    uint outUavIndex; // bindless UAV 槽
    uint width;
    uint height;
    uint frameIndex;
    uint4 padding;
};

ConstantBuffer<ComputeParams> g_Compute : register(b0);

[numthreads(8, 8, 1)]
void ComputeSmokeCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_Compute.width || id.y >= g_Compute.height)
        return;

    RWTexture2D<float4> outTex = ResourceDescriptorHeap[g_Compute.outUavIndex];

    float2 uv = (id.xy + 0.5f) / float2(g_Compute.width, g_Compute.height);
    float t = g_Compute.frameIndex * 0.05f;
    outTex[id.xy] = float4(uv.x, uv.y, 0.5f + 0.5f * sin(t), 1.0f);
}