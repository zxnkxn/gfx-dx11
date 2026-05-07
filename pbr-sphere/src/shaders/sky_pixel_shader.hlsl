cbuffer SkyFrameConstants : register(b0)
{
    row_major float4x4 viewProjection;
    float4 cameraPosition;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
};

TextureCube<float4> environmentMap : register(t0);
SamplerState linearClampSampler : register(s0);

float4 PS(PSInput input) : SV_Target
{
    const float3 direction = normalize(input.worldPosition - cameraPosition.xyz);
    const float3 environmentColor = environmentMap.SampleLevel(linearClampSampler, direction, 0.0f).rgb;
    const float3 gammaCorrectedColor = pow(clamp(environmentColor, 0.0f.xxx, 1.0f.xxx), 1.0f / 2.2f);
    return float4(gammaCorrectedColor, 1.0f);
}
