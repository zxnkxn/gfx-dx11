cbuffer DownsampleConstants : register(b0)
{
    float2 sourceTexelSize;
    float2 padding;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

Texture2D<float> sourceLuminance : register(t0);
SamplerState pointClampSampler : register(s0);

float SampleValue(float2 uv)
{
    return max(sourceLuminance.SampleLevel(pointClampSampler, uv, 0.0f), 0.0f);
}

float PS(PSInput input) : SV_Target
{
    const float2 offset = sourceTexelSize * 0.5f;

    float value = 0.0f;
    value += SampleValue(input.texCoord + float2(-offset.x, -offset.y));
    value += SampleValue(input.texCoord + float2(offset.x, -offset.y));
    value += SampleValue(input.texCoord + float2(-offset.x, offset.y));
    value += SampleValue(input.texCoord + float2(offset.x, offset.y));

    return value * 0.25f;
}
