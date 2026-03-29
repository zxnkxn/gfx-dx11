cbuffer AdaptationConstants : register(b0)
{
    float deltaTime;
    float adaptationRate;
    float minLuminance;
    float padding;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

Texture2D<float> currentLogAverage : register(t0);
Texture2D<float> previousAdapted : register(t1);
SamplerState pointClampSampler : register(s0);

float PS(PSInput input) : SV_Target
{
    const float currentAverageLuminance =
        exp(currentLogAverage.SampleLevel(pointClampSampler, float2(0.5f, 0.5f), 0.0f));

    const float previousAverageLuminance =
        max(previousAdapted.SampleLevel(pointClampSampler, float2(0.5f, 0.5f), 0.0f), minLuminance);

    const float adaptationFactor = 1.0f - exp(-deltaTime * adaptationRate);
    return lerp(previousAverageLuminance, max(currentAverageLuminance, minLuminance), adaptationFactor);
}
