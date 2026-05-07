cbuffer PostProcessConstants : register(b0)
{
    float2 inverseTextureSize;
    float2 blurDirection;
    float4 parameters;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

Texture2D<float4> sceneTexture : register(t0);
Texture2D<float4> bloomTexture : register(t1);
Texture2D<float4> bloomSourceTexture : register(t2);
SamplerState linearClampSampler : register(s0);

float3 ToneMapReinhard(float3 color)
{
    return color / (1.0f.xxx + color);
}

float4 PS(PSInput input) : SV_Target
{
    const float bloomIntensity = parameters.x;
    const float exposure = parameters.y;

    const float3 sceneColor = sceneTexture.Sample(linearClampSampler, input.texCoord).rgb;
    const float3 blurredBloom = bloomTexture.Sample(linearClampSampler, input.texCoord).rgb;
    const float3 sourceBloom = bloomSourceTexture.Sample(linearClampSampler, input.texCoord).rgb;
    const float3 bloomHalo = max(blurredBloom - sourceBloom, 0.0f.xxx) * bloomIntensity;
    const float3 toneMappedColor = ToneMapReinhard(max((sceneColor + bloomHalo) * exposure, 0.0f.xxx));
    const float3 gammaCorrectedColor = pow(clamp(toneMappedColor, 0.0f.xxx, 1.0f.xxx), 1.0f / 2.2f);
    return float4(gammaCorrectedColor, 1.0f);
}
