static const float kPi = 3.14159265f;
static const float kInvPi = 0.318309886f;
static const float kInvTwoPi = 0.159154943f;

struct PSInput
{
    float4 position : SV_POSITION;
    float3 direction : TEXCOORD0;
};

Texture2D<float4> equirectangularTexture : register(t0);
SamplerState linearClampSampler : register(s0);

float2 DirectionToEquirectangularUv(float3 direction)
{
    const float3 normalizedDirection = normalize(direction);
    const float u = atan2(normalizedDirection.z, normalizedDirection.x) * kInvTwoPi + 0.5f;
    const float v = 0.5f - asin(clamp(normalizedDirection.y, -1.0f, 1.0f)) * kInvPi;
    return float2(u, v);
}

float4 PS(PSInput input) : SV_Target
{
    const float2 uv = DirectionToEquirectangularUv(input.direction);
    const float3 color = max(equirectangularTexture.SampleLevel(linearClampSampler, uv, 0.0f).rgb, 0.0f.xxx);
    return float4(color, 1.0f);
}
