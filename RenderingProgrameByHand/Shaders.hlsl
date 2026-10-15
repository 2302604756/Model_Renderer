// 对应根签名的 32-bit Root Constants
struct PushConstants
{
    uint textureIndex;
    uint padding[3];
};


ConstantBuffer<PushConstants> g_Push : register(b0);
SamplerState g_LinearSampler : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

PSInput VSMain(VSInput vin)
{
    PSInput vout;
    vout.position = float4(vin.position, 1.0f);
    vout.uv = vin.uv;
    return vout;
}

float4 PSMain(PSInput pin) : SV_Target
{
    // 关键特性：直接从全局着色器可见堆解引用
    Texture2D diffuseTex = ResourceDescriptorHeap[g_Push.textureIndex];
    return diffuseTex.Sample(g_LinearSampler, pin.uv);
}