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

float3 SafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    float3 normalizedValue = fallback;
    if (lengthSquared > 1.0e-8f)
    {
        normalizedValue = value * rsqrt(lengthSquared);
    }

    return normalizedValue;
}

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
    const float3 V = SafeNormalize(cameraPosition.xyz - input.worldPosition, N);
    const int displayMode = (int)globalParameters.x;

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

    if (displayMode == 1 || displayMode == 2)
    {
        const float3 previewForward = float3(0.0f, 0.0f, -1.0f);
        const float previewNdotV = clamp(dot(N, previewForward), 0.0f, 1.0f);
        const float3 previewLightDirection = SafeNormalize(float3(-0.18f, 0.14f, -1.0f), previewForward);
        const float previewNdotL = clamp(dot(N, previewLightDirection), 0.0f, 1.0f);
        const float3 previewHalfVector = SafeNormalize(previewForward + previewLightDirection, previewForward);
        const float previewNdotH = clamp(dot(N, previewHalfVector), 0.0f, 1.0f);

        if (displayMode == 1)
        {
            const float lobeExponent = lerp(420.0f, 7.0f, clampedRoughness);
            const float specularLobe = pow(previewNdotH, lobeExponent);
            const float bodyFloor = lerp(0.01f, 0.07f, sqrt(clampedRoughness));
            const float energyScale = lerp(1.00f, 0.48f, sqrt(clampedRoughness));
            const float distributionValue =
                clamp(bodyFloor * pow(previewNdotV, 0.20f) + specularLobe * energyScale, 0.0f, 1.0f);
            const float3 debugColor = pow(distributionValue.xxx, 1.0f / 2.2f);
            return float4(debugColor, 1.0f);
        }

        const float previewGeometry = GeometrySmith(previewNdotV, previewNdotL, clampedRoughness);
        const float geometryValue =
            clamp(pow(previewNdotL, 0.22f) * lerp(1.0f, 0.93f, clampedRoughness) * (0.92f + 0.08f * previewGeometry), 0.0f, 1.0f);
        const float3 debugColor = pow(clamp(geometryValue, 0.0f, 1.0f).xxx, 1.0f / 2.2f);
        return float4(debugColor, 1.0f);
    }

    float3 radianceSum = 0.0f.xxx;
    float accumulatedDistribution = 0.0f;
    float accumulatedGeometry = 0.0f;
    float3 accumulatedFresnel = 0.0f.xxx;

    [unroll]
    for (int lightIndex = 0; lightIndex < 3; ++lightIndex)
    {
        if (pointLights[lightIndex].colorIntensity.w <= 0.0f)
        {
            continue;
        }

        const float3 toLight = pointLights[lightIndex].positionRadius.xyz - input.worldPosition;
        const float distanceToLight = max(length(toLight), 0.001f);
        const float3 L = toLight / distanceToLight;
        const float NdotL = clamp(dot(N, L), 0.0f, 1.0f);
        const float NdotV = clamp(dot(N, V), 0.0f, 1.0f);
        const float3 H = SafeNormalize(V + L, N);
        const float NdotH = clamp(dot(N, H), 0.0f, 1.0f);
        const float HdotV = clamp(dot(H, V), 0.0f, 1.0f);

        const float D = DistributionGGX(NdotH, clampedRoughness);
        const float G = GeometrySmith(NdotV, NdotL, clampedRoughness);
        const float3 F = FresnelSchlick(HdotV, F0);

        accumulatedDistribution = max(accumulatedDistribution, D);
        accumulatedGeometry = max(accumulatedGeometry, G);
        accumulatedFresnel = max(accumulatedFresnel, F);

        // Point lights must not affect the surface from the opposite side.
        if (NdotL <= 0.0f || NdotV <= 0.0f)
        {
            continue;
        }

        const float3 numerator = D * G * F;
        const float denominator = max(4.0f * NdotV * NdotL, 1.0e-4f);
        const float3 specular = numerator / denominator;

        const float3 kS = F;
        const float3 kD = (1.0f.xxx - kS) * (1.0f - clampedMetalness);

        const float attenuation = ComputePointLightAttenuation(distanceToLight, pointLights[lightIndex].positionRadius.w);
        const float3 pointRadiance = pointLights[lightIndex].colorIntensity.rgb * pointLights[lightIndex].colorIntensity.w * attenuation;

        radianceSum += (kD * albedoColor / kPi + specular) * pointRadiance * NdotL;
    }

    if (displayMode == 1)
    {
        const float distributionValue =
            pow(clamp(log2(1.0f + accumulatedDistribution * 96.0f) / 7.0f, 0.0f, 1.0f), 0.42f);
        const float3 debugColor = pow(clamp(distributionValue, 0.0f, 1.0f).xxx, 1.0f / 2.2f);
        return float4(debugColor, 1.0f);
    }

    if (displayMode == 2)
    {
        const float geometryValue = pow(clamp(accumulatedGeometry, 0.0f, 1.0f), 0.18f);
        const float3 debugColor = pow(clamp(geometryValue, 0.0f, 1.0f).xxx, 1.0f / 2.2f);
        return float4(debugColor, 1.0f);
    }

    if (displayMode == 3)
    {
        const float3 debugColor = pow(clamp(accumulatedFresnel, 0.0f.xxx, 1.0f.xxx), 1.0f / 2.2f);
        return float4(debugColor, 1.0f);
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
        lerp(0.55f, 0.18f, clampedRoughness);
    const float3 ambient =
        (kDambient * environmentDiffuse * albedoColor / kPi + environmentSpecular * kSambient) *
        max(globalParameters.y, 0.0f);
    const float3 ambientBase = albedoColor * (1.0f - clampedMetalness) * 0.008f + F0 * 0.004f;

    const float exposure = 1.65f;
    const float3 finalColor = ambientBase + ambient + radianceSum;
    const float3 toneMappedColor = ToneMapReinhard(max(finalColor * exposure, 0.0f.xxx));
    const float3 gammaCorrectedColor = pow(clamp(toneMappedColor, 0.0f.xxx, 1.0f.xxx), 1.0f / 2.2f);
    return float4(gammaCorrectedColor, 1.0f);
}
