static const float kPi = 3.14159265f;

struct PointLightData
{
    float4 positionRadius;
    float4 colorIntensity;
};

cbuffer SceneFrameConstants : register(b0)
{
    row_major float4x4 viewProjection;
    float4 cameraPosition;
    PointLightData pointLights[3];
    float4 globalParameters;
};

cbuffer SceneObjectConstants : register(b1)
{
    row_major float4x4 world;
    row_major float4x4 normalMatrix;
    float4 albedo;
    float4 materialParameters;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 albedo : TEXCOORD2;
    float roughness : TEXCOORD3;
    float metalness : TEXCOORD4;
};

TextureCube<float4> environmentMap : register(t0);
SamplerState linearClampSampler : register(s0);

float DistributionGGX(float NdotH, float roughness)
{
    const float clampedRoughness = clamp(roughness, 0.045f, 1.0f);
    const float a = clampedRoughness * clampedRoughness;
    const float a2 = a * a;
    const float clampedNdotH = clamp(NdotH, 0.0f, 1.0f);
    const float denominator = clampedNdotH * clampedNdotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(kPi * denominator * denominator, 1.0e-4f);
}

float GeometrySchlickGGX(float NdotX, float roughness)
{
    const float clampedRoughness = clamp(roughness, 0.045f, 1.0f);
    const float r = clampedRoughness + 1.0f;
    const float k = (r * r) / 8.0f;
    const float clampedNdotX = clamp(NdotX, 0.0f, 1.0f);
    return clampedNdotX / max(clampedNdotX * (1.0f - k) + k, 1.0e-4f);
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    const float ggxView = GeometrySchlickGGX(NdotV, roughness);
    const float ggxLight = GeometrySchlickGGX(NdotL, roughness);
    return ggxView * ggxLight;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    const float clampedCosTheta = clamp(cosTheta, 0.0f, 1.0f);
    return F0 + (1.0f.xxx - F0) * pow(1.0f - clampedCosTheta, 5.0f);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    const float clampedCosTheta = clamp(cosTheta, 0.0f, 1.0f);
    const float3 grazingF0 = max((1.0f - roughness).xxx, F0);
    return F0 + (grazingF0 - F0) * pow(1.0f - clampedCosTheta, 5.0f);
}

float3 ToneMapReinhard(float3 color)
{
    return color / (1.0f.xxx + color);
}

float ComputePointLightAttenuation(float distanceToLight, float radius)
{
    const float clampedRadius = max(radius, 0.001f);
    const float normalizedDistance = clamp(distanceToLight / clampedRadius, 0.0f, 1.0f);
    const float rangeFalloff = 1.0f - normalizedDistance;
    return (rangeFalloff * rangeFalloff) / max(distanceToLight * distanceToLight, 0.25f);
}

float4 PS(PSInput input) : SV_Target
{
    const float3 N = normalize(input.normal);
    const float3 V = normalize(cameraPosition.xyz - input.worldPosition);

    const float clampedRoughness = clamp(input.roughness, 0.045f, 1.0f);
    const float clampedMetalness = clamp(input.metalness, 0.0f, 1.0f);
    const float3 albedoColor = clamp(input.albedo, 0.0f.xxx, 1.0f.xxx);
    const float emissiveStrength = max(materialParameters.z, 0.0f);

    const float3 dielectricF0 = float3(0.04f, 0.04f, 0.04f);
    const float3 F0 = lerp(dielectricF0, albedoColor, clampedMetalness);

    if (emissiveStrength > 0.0f)
    {
        const float3 emissiveColor = ToneMapReinhard(albedoColor * emissiveStrength);
        const float3 gammaCorrectedEmissive = pow(clamp(emissiveColor, 0.0f.xxx, 1.0f.xxx), 1.0f / 2.2f);
        return float4(gammaCorrectedEmissive, 1.0f);
    }

    float3 radianceSum = 0.0f.xxx;
    float accumulatedDistribution = 0.0f;
    float accumulatedGeometry = 0.0f;
    float3 accumulatedFresnel = 0.0f.xxx;
    const float viewGeometry = GeometrySchlickGGX(clamp(dot(N, V), 0.0f, 1.0f), clampedRoughness);

    [unroll]
    for (int lightIndex = 0; lightIndex < 3; ++lightIndex)
    {
        const float3 toLight = pointLights[lightIndex].positionRadius.xyz - input.worldPosition;
        const float distanceToLight = max(length(toLight), 0.001f);
        const float3 L = toLight / distanceToLight;
        const float3 H = normalize(V + L);

        const float NdotL = clamp(dot(N, L), 0.0f, 1.0f);
        const float NdotV = clamp(dot(N, V), 0.0f, 1.0f);
        const float NdotH = clamp(dot(N, H), 0.0f, 1.0f);
        const float HdotV = clamp(dot(H, V), 0.0f, 1.0f);

        const float D = DistributionGGX(NdotH, clampedRoughness);
        const float lightGeometry = GeometrySchlickGGX(NdotL, clampedRoughness);
        const float G = viewGeometry * lightGeometry;
        const float3 F = FresnelSchlick(HdotV, F0);

        accumulatedDistribution = max(accumulatedDistribution, D);
        // Use a debug-friendly visibility measure instead of the raw product only.
        accumulatedGeometry = max(accumulatedGeometry, 0.5f * (viewGeometry + lightGeometry));
        accumulatedFresnel = max(accumulatedFresnel, F);

        const float3 numerator = D * G * F;
        const float denominator = max(4.0f * NdotV * NdotL, 1.0e-4f);
        const float3 specular = numerator / denominator;

        const float3 kS = F;
        const float3 kD = (1.0f.xxx - kS) * (1.0f - clampedMetalness);

        const float attenuation = ComputePointLightAttenuation(distanceToLight, pointLights[lightIndex].positionRadius.w);
        const float3 pointRadiance = pointLights[lightIndex].colorIntensity.rgb * pointLights[lightIndex].colorIntensity.w * attenuation;

        radianceSum += (kD * albedoColor / kPi + specular) * pointRadiance * NdotL;
    }

    const float NdotV = clamp(dot(N, V), 0.0f, 1.0f);
    const float3 Fambient = FresnelSchlickRoughness(NdotV, F0, clampedRoughness);
    const float3 kSambient = Fambient;
    const float3 kDambient = (1.0f.xxx - kSambient) * (1.0f - clampedMetalness);

    const float maxEnvironmentMipLevel = 6.0f;
    const float3 environmentDiffuse = environmentMap.SampleLevel(linearClampSampler, N, maxEnvironmentMipLevel).rgb;
    const float3 reflectionDirection = reflect(-V, N);
    const float3 environmentSpecular =
        environmentMap.SampleLevel(linearClampSampler, reflectionDirection, clampedRoughness * maxEnvironmentMipLevel).rgb *
        lerp(1.15f, 0.35f, clampedRoughness);
    const float3 ambient =
        (kDambient * environmentDiffuse * albedoColor / kPi + environmentSpecular * kSambient) *
        max(globalParameters.y, 0.0f);
    const float3 ambientBase = albedoColor * (1.0f - clampedMetalness) * 0.02f;

    const int displayMode = (int)globalParameters.x;
    if (displayMode == 1)
    {
        const float distributionValue =
            pow(clamp(log2(1.0f + accumulatedDistribution * 8.0f) / 3.6f, 0.0f, 1.0f), 0.6f);
        return float4(distributionValue.xxx, 1.0f);
    }

    if (displayMode == 2)
    {
        const float geometryValue = lerp(0.15f, 1.0f, pow(clamp(accumulatedGeometry, 0.0f, 1.0f), 0.8f));
        return float4(geometryValue.xxx, 1.0f);
    }

    if (displayMode == 3)
    {
        const float fresnelValue =
            pow(dot(clamp(accumulatedFresnel, 0.0f.xxx, 1.0f.xxx), float3(0.2126f, 0.7152f, 0.0722f)), 0.45f);
        return float4(fresnelValue.xxx, 1.0f);
    }

    const float exposure = 1.2f;
    const float3 finalColor = ambientBase + ambient + radianceSum;
    const float3 toneMappedColor = ToneMapReinhard(max(finalColor * exposure, 0.0f.xxx));
    const float3 gammaCorrectedColor = pow(clamp(toneMappedColor, 0.0f.xxx, 1.0f.xxx), 1.0f / 2.2f);
    return float4(gammaCorrectedColor, 1.0f);
}
