static const float kPi = 3.14159265f;
static const int kPhiSampleCount = 100;
static const int kThetaSampleCount = 25;

struct PSInput
{
    float4 position : SV_POSITION;
    float3 direction : TEXCOORD0;
};

TextureCube<float4> environmentMap : register(t0);
SamplerState linearClampSampler : register(s0);

float4 PS(PSInput input) : SV_Target
{
    const float3 normal = normalize(input.direction);

    // Build a tangent basis around the normal to integrate the whole upper hemisphere.
    const float3 referenceDirection =
        (abs(normal.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent = normalize(cross(referenceDirection, normal));
    const float3 binormal = cross(normal, tangent);

    float3 irradiance = 0.0f.xxx;

    [loop]
    for (int phiIndex = 0; phiIndex < kPhiSampleCount; ++phiIndex)
    {
        [loop]
        for (int thetaIndex = 0; thetaIndex < kThetaSampleCount; ++thetaIndex)
        {
            const float phi = (phiIndex + 0.5f) * (2.0f * kPi / kPhiSampleCount);
            const float theta = (thetaIndex + 0.5f) * (0.5f * kPi / kThetaSampleCount);

            const float3 tangentSample = float3(
                sin(theta) * cos(phi),
                sin(theta) * sin(phi),
                cos(theta));

            const float3 sampleDirection =
                tangentSample.x * tangent +
                tangentSample.y * binormal +
                tangentSample.z * normal;

            irradiance +=
                environmentMap.SampleLevel(linearClampSampler, normalize(sampleDirection), 0.0f).rgb *
                cos(theta) *
                sin(theta);
        }
    }

    irradiance = kPi * irradiance / (kPhiSampleCount * kThetaSampleCount);
    return float4(max(irradiance, 0.0f.xxx), 1.0f);
}
