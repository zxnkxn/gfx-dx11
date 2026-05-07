#define SSAO_SAMPLE_COUNT 32
#define SSAO_NOISE_COUNT 16

cbuffer SsaoConstants : register(b0)
{
    row_major float4x4 projection;
    row_major float4x4 inverseProjection;
    float4 ssaoParameters;
    float4 inverseTextureSize;
    float4 ssaoSamples[SSAO_SAMPLE_COUNT];
    float4 ssaoNoise[SSAO_NOISE_COUNT];
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

Texture2D<float> depthTexture : register(t0);
Texture2D<float4> normalTexture : register(t1);
SamplerState pointClampSampler : register(s0);

float2 UVToNdc(float2 uv)
{
    return float2(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f);
}

float2 NdcToUV(float2 ndc)
{
    return float2(ndc.x * 0.5f + 0.5f, 1.0f - (ndc.y * 0.5f + 0.5f));
}

float3 ReconstructViewPosition(float2 uv, float depth)
{
    const float4 ndc = float4(UVToNdc(uv), depth, 1.0f);
    const float4 viewPosition = mul(ndc, inverseProjection);
    return viewPosition.xyz / max(viewPosition.w, 1.0e-5f);
}

float4 PS(PSInput input) : SV_Target
{
    const float2 uv = input.texCoord;
    const float depth = depthTexture.Sample(pointClampSampler, uv);
    if (depth >= 0.9999f)
    {
        return 1.0f;
    }

    const float3 viewPosition = ReconstructViewPosition(uv, depth);
    const float3 normal = normalize(normalTexture.Sample(pointClampSampler, uv).xyz * 2.0f - 1.0f);

    const uint2 noiseCoord = uint2(input.position.xy) & 3u;
    float3 randomVector = ssaoNoise[noiseCoord.y * 4u + noiseCoord.x].xyz;
    if (dot(randomVector, randomVector) < 1.0e-4f)
    {
        randomVector = float3(1.0f, 0.0f, 0.0f);
    }

    float3 tangent = randomVector - normal * dot(randomVector, normal);
    if (dot(tangent, tangent) < 1.0e-4f)
    {
        const float3 fallback = abs(normal.x - 1.0f) > 0.01f ? float3(1.0f, 0.0f, 0.0f) : float3(0.0f, 1.0f, 0.0f);
        tangent = fallback - normal * dot(fallback, normal);
    }

    tangent = normalize(tangent);
    const float3 bitangent = normalize(cross(normal, tangent));

    float occlusion = 0.0f;

    [unroll]
    for (int sampleIndex = 0; sampleIndex < SSAO_SAMPLE_COUNT; ++sampleIndex)
    {
        const float3 sampleDirection =
            ssaoSamples[sampleIndex].x * tangent +
            ssaoSamples[sampleIndex].y * bitangent +
            ssaoSamples[sampleIndex].z * normal;
        const float3 samplePosition = viewPosition + sampleDirection * ssaoParameters.x;

        const float4 projectedSample = mul(float4(samplePosition, 1.0f), projection);
        if (projectedSample.w <= 1.0e-5f)
        {
            continue;
        }

        const float3 sampleNdc = projectedSample.xyz / projectedSample.w;
        const float2 sampleUv = NdcToUV(sampleNdc.xy);
        if (any(sampleUv < 0.0f.xx) || any(sampleUv > 1.0f.xx))
        {
            continue;
        }

        const float sampleDepth = depthTexture.Sample(pointClampSampler, sampleUv);
        if (sampleDepth >= 0.9999f)
        {
            continue;
        }

        const float3 occluderViewPosition = ReconstructViewPosition(sampleUv, sampleDepth);
        const float depthDelta = samplePosition.z - occluderViewPosition.z;
        if (depthDelta > ssaoParameters.y && depthDelta < ssaoParameters.w)
        {
            const float rangeWeight = saturate(1.0f - depthDelta / ssaoParameters.w);
            occlusion += rangeWeight;
        }
    }

    const float visibility = 1.0f - (occlusion / SSAO_SAMPLE_COUNT) * ssaoParameters.z;
    return saturate(visibility);
}
