static const float kPi = 3.14159265f;
static const uint kSampleCount = 1024u;

struct PSInput
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

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

float GeometrySchlickGGX(float NdotX, float roughness)
{
    const float k = (roughness * roughness) * 0.5f;
    return NdotX / max(NdotX * (1.0f - k) + k, 1.0e-4f);
}

float GeometrySmith(float3 normal, float3 viewDirection, float3 lightDirection, float roughness)
{
    const float NdotV = saturate(dot(normal, viewDirection));
    const float NdotL = saturate(dot(normal, lightDirection));
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

float2 IntegrateBRDF(float NdotV, float roughness)
{
    const float3 normal = float3(0.0f, 0.0f, 1.0f);
    float3 viewDirection = 0.0f.xxx;
    viewDirection.x = sqrt(max(1.0f - NdotV * NdotV, 0.0f));
    viewDirection.z = NdotV;

    float integratedScale = 0.0f;
    float integratedBias = 0.0f;

    [loop]
    for (uint i = 0u; i < kSampleCount; ++i)
    {
        const float2 Xi = Hammersley(i, kSampleCount);
        const float3 halfVector = ImportanceSampleGGX(Xi, normal, roughness);
        const float3 lightDirection = normalize(2.0f * dot(viewDirection, halfVector) * halfVector - viewDirection);

        const float NdotL = saturate(lightDirection.z);
        const float NdotH = saturate(halfVector.z);
        const float VdotH = saturate(dot(viewDirection, halfVector));
        if (NdotL <= 0.0f)
        {
            continue;
        }

        const float geometry = GeometrySmith(normal, viewDirection, lightDirection, roughness);
        const float visibility = (geometry * VdotH) / max(NdotH * NdotV, 1.0e-4f);
        const float fresnel = pow(1.0f - VdotH, 5.0f);

        integratedScale += (1.0f - fresnel) * visibility;
        integratedBias += fresnel * visibility;
    }

    return float2(integratedScale, integratedBias) / float(kSampleCount);
}

float4 PS(PSInput input) : SV_Target
{
    const float2 uv = saturate(input.texCoord);
    const float2 integratedBrdf = IntegrateBRDF(uv.x, uv.y);
    return float4(integratedBrdf, 0.0f, 1.0f);
}
