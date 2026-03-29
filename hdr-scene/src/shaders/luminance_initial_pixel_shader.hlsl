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

Texture2D<float4> hdrTexture : register(t0);
SamplerState pointClampSampler : register(s0);

float SampleLogLuminance(float2 uv)
{
    const float3 color = hdrTexture.SampleLevel(pointClampSampler, uv, 0.0f).rgb;
    const float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    return log(max(luminance, 1.0e-4f));
}

float PS(PSInput input) : SV_Target
{
    const float2 offset = sourceTexelSize * 0.5f;

    float value = 0.0f;
    value += SampleLogLuminance(input.texCoord + float2(-offset.x, -offset.y));
    value += SampleLogLuminance(input.texCoord + float2(offset.x, -offset.y));
    value += SampleLogLuminance(input.texCoord + float2(-offset.x, offset.y));
    value += SampleLogLuminance(input.texCoord + float2(offset.x, offset.y));

    return value * 0.25f;
}
