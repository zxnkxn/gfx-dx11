static const float kPi = 3.14159265f;
static const uint kSampleCount = 1024u;

cbuffer CaptureConstants : register(b0)
{
    float4 faceForward;
    float4 faceRight;
    float4 faceUp;
    float4 prefilterParameters;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 direction : TEXCOORD0;
};

TextureCube<float4> environmentMap : register(t0);
SamplerState linearClampSampler : register(s0);

float RadicalInverseVanDerCorput(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float2 Hammersley(uint index, uint count)
{
    return float2(float(index) / float(count), RadicalInverseVanDerCorput(index));
}

float DistributionGGX(float NdotH, float roughness)
{
    const float alpha = roughness * roughness;
    const float alphaSquared = alpha * alpha;
    const float denominator = max(NdotH * NdotH * (alphaSquared - 1.0f) + 1.0f, 1.0e-4f);
    return alphaSquared / max(kPi * denominator * denominator, 1.0e-4f);
}

float3 ImportanceSampleGGX(float2 Xi, float3 normal, float roughness)
{
    const float alpha = roughness * roughness;
    const float phi = 2.0f * kPi * Xi.x;
    const float cosTheta = sqrt((1.0f - Xi.y) / max(1.0f + (alpha * alpha - 1.0f) * Xi.y, 1.0e-4f));
    const float sinTheta = sqrt(max(1.0f - cosTheta * cosTheta, 0.0f));

    const float3 halfVector = float3(
        cos(phi) * sinTheta,
        sin(phi) * sinTheta,
        cosTheta);

    const float3 up = (abs(normal.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent = normalize(cross(up, normal));
    const float3 bitangent = cross(normal, tangent);

    return normalize(
        tangent * halfVector.x +
        bitangent * halfVector.y +
        normal * halfVector.z);
}

float4 PS(PSInput input) : SV_Target
{
    const float roughness = saturate(prefilterParameters.x);
    const float sourceCubemapFaceSize = max(prefilterParameters.y, 1.0f);

    const float3 N = normalize(input.direction);
    const float3 R = N;
    const float3 V = R;

    float3 prefilteredColor = 0.0f.xxx;
    float totalWeight = 0.0f;

    [loop]
    for (uint i = 0u; i < kSampleCount; ++i)
    {
        const float2 Xi = Hammersley(i, kSampleCount);
        const float3 H = ImportanceSampleGGX(Xi, N, roughness);
        const float3 L = normalize(2.0f * dot(V, H) * H - V);

        const float NdotL = saturate(dot(N, L));
        if (NdotL <= 0.0f)
        {
            continue;
        }

        const float NdotH = saturate(dot(N, H));
        const float HdotV = saturate(dot(H, V));
        const float pdf = max(DistributionGGX(NdotH, roughness) * NdotH / max(4.0f * HdotV, 1.0e-4f), 1.0e-4f);
        const float texelSolidAngle = 4.0f * kPi / (6.0f * sourceCubemapFaceSize * sourceCubemapFaceSize);
        const float sampleSolidAngle = 1.0f / max(float(kSampleCount) * pdf, 1.0e-4f);
        const float mipLevel =
            (roughness <= 0.0f)
            ? 0.0f
            : max(0.5f * log2(sampleSolidAngle / texelSolidAngle), 0.0f);

        prefilteredColor += environmentMap.SampleLevel(linearClampSampler, L, mipLevel).rgb * NdotL;
        totalWeight += NdotL;
    }

    prefilteredColor /= max(totalWeight, 1.0e-4f);
    return float4(max(prefilteredColor, 0.0f.xxx), 1.0f);
}
