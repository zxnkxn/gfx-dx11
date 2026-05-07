cbuffer ToneMappingConstants : register(b0)
{
    float middleGray;
    float whitePoint;
    float minLuminance;
    float padding;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

Texture2D<float4> hdrTexture : register(t0);
Texture2D<float> adaptedLuminance : register(t1);
SamplerState linearClampSampler : register(s0);
SamplerState pointClampSampler : register(s1);

float3 Uncharted2Tonemap(float3 x)
{
    const float A = 0.15f;
    const float B = 0.50f;
    const float C = 0.10f;
    const float D = 0.20f;
    const float E = 0.02f;
    const float F = 0.30f;

    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float4 PS(PSInput input) : SV_Target
{
    const float3 hdrColor = hdrTexture.Sample(linearClampSampler, input.texCoord).rgb;
    const float averageLuminance =
        max(adaptedLuminance.SampleLevel(pointClampSampler, float2(0.5f, 0.5f), 0.0f), minLuminance);

    const float exposure = middleGray / averageLuminance;
    const float3 toneMappedColor = Uncharted2Tonemap(hdrColor * exposure);
    const float3 whiteScale = 1.0f / Uncharted2Tonemap(whitePoint.xxx);
    const float3 mappedColor = pow(saturate(toneMappedColor * whiteScale), 1.0f / 2.2f);

    return float4(mappedColor, 1.0f);
}
