static const float kPi = 3.14159265f;
static const float kDirectDiffuseBoost = 6.0f;
static const float kDirectSpecularBoost = 0.0f;

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

TextureCube<float4> irradianceMap : register(t0);
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
    return rangeFalloff * rangeFalloff;
}

float4 PS(PSInput input) : SV_Target
{
    const float3 V = normalize(cameraPosition.xyz - input.worldPosition);
    const float3 geometricNormal = normalize(input.normal);
    // Keep the shading normal oriented towards the camera. This makes the
    // direct-light branch robust even if the visible sphere faces end up using
    // the opposite winding/normal orientation.
    const float3 N = (dot(geometricNormal, V) >= 0.0f) ? geometricNormal : -geometricNormal;
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

    if (displayMode != 0 && displayMode != 4)
    {
        // Use a stable analytic preview for the BRDF terms so the debug modes
        // stay readable and are not tied to the scene lighting composition.
        const float3 debugNormal = (dot(N, V) >= 0.0f) ? N : -N;
        const float3 debugViewDirection = normalize(V);
        const float3 referenceUp = (abs(debugViewDirection.y) < 0.95f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
        const float3 debugTangent = normalize(cross(referenceUp, debugViewDirection));
        const float3 debugBitangent = normalize(cross(debugViewDirection, debugTangent));

        const float debugNdotV = clamp(dot(debugNormal, debugViewDirection), 0.0f, 1.0f);

        const float3 debugNdfLightDirection =
            normalize(debugViewDirection * 0.80f - debugTangent * 0.52f + debugBitangent * 0.30f);
        const float3 debugNdfHalfVector = normalize(debugViewDirection + debugNdfLightDirection);
        const float debugNdfNdotH = clamp(dot(debugNormal, debugNdfHalfVector), 0.0f, 1.0f);
        const float debugDistribution = DistributionGGX(debugNdfNdotH, clampedRoughness);

        const float3 debugGeometryLightDirection =
            normalize(debugViewDirection * 0.32f - debugTangent * 0.88f + debugBitangent * 0.18f);
        const float debugGeometryNdotL = clamp(dot(debugNormal, debugGeometryLightDirection), 0.0f, 1.0f);
        const float debugGeometry = GeometrySmith(debugNdotV, debugGeometryNdotL, clampedRoughness);

        const float3 debugFresnelLightDirection =
            normalize(-debugViewDirection * 0.45f - debugTangent * 0.82f + debugBitangent * 0.35f);
        const float3 debugFresnelHalfVector = normalize(debugViewDirection + debugFresnelLightDirection);
        const float debugFresnelHdotV = clamp(dot(debugFresnelHalfVector, debugViewDirection), 0.0f, 1.0f);
        const float3 debugF0 = lerp(float3(0.03f, 0.14f, 0.72f), albedoColor, clampedMetalness);
        const float3 debugFresnel = FresnelSchlick(debugFresnelHdotV, debugF0);

        if (displayMode == 1)
        {
            const float distributionValue =
                pow(clamp(log2(1.0f + debugDistribution * 96.0f) / 7.0f, 0.0f, 1.0f), 0.42f);
            const float3 debugColor = pow(lerp(0.08f.xxx, 1.0f.xxx, distributionValue), 1.0f / 2.2f);
            return float4(debugColor, 1.0f);
        }

        if (displayMode == 2)
        {
            const float geometryValue = pow(clamp(debugGeometry, 0.0f, 1.0f), 0.55f);
            const float3 debugColor = pow(lerp(0.08f.xxx, 1.0f.xxx, geometryValue), 1.0f / 2.2f);
            return float4(debugColor, 1.0f);
        }

        const float3 debugColor = pow(clamp(debugFresnel, 0.0f.xxx, 1.0f.xxx), 1.0f / 2.2f);
        return float4(debugColor, 1.0f);
    }

    float3 radianceSum = 0.0f.xxx;
    float accumulatedDistribution = 0.0f;
    float accumulatedGeometry = 0.0f;
    float3 accumulatedFresnel = 0.0f.xxx;

    [unroll]
    for (int lightIndex = 0; lightIndex < 3; ++lightIndex)
    {
        const float3 toLight = pointLights[lightIndex].positionRadius.xyz - input.worldPosition;
        const float distanceToLight = max(length(toLight), 0.001f);
        const float3 L = toLight / distanceToLight;
        const float3 H = normalize(V + L);

        const float NdotL = clamp(dot(N, L), 0.0f, 1.0f);
        const float NdotV = clamp(dot(N, V), 0.0f, 1.0f);

        // Point lights must not affect the surface from the opposite side.
        if (NdotL <= 0.0f || NdotV <= 0.0f)
        {
            continue;
        }

        const float NdotH = clamp(dot(N, H), 0.0f, 1.0f);
        const float HdotV = clamp(dot(H, V), 0.0f, 1.0f);

        const float directLightRoughness = 1.0f;
        const float D = DistributionGGX(NdotH, directLightRoughness);
        const float G = GeometrySmith(NdotV, NdotL, directLightRoughness);
        const float3 F = FresnelSchlick(HdotV, F0);

        accumulatedDistribution = max(accumulatedDistribution, D);
        accumulatedGeometry = max(accumulatedGeometry, G);
        accumulatedFresnel = max(accumulatedFresnel, F);

        const float3 numerator = D * G * F;
        const float denominator = max(4.0f * NdotV * NdotL, 1.0e-4f);
        const float3 specular = numerator / denominator;

        const float3 kS = F;
        const float3 kD = (1.0f.xxx - kS) * (1.0f - clampedMetalness);

        const float attenuation = ComputePointLightAttenuation(distanceToLight, pointLights[lightIndex].positionRadius.w);
        const float3 pointRadiance =
            pointLights[lightIndex].colorIntensity.rgb *
            pointLights[lightIndex].colorIntensity.w *
            attenuation;

        const float3 directDiffuse = kD * albedoColor / kPi;
        const float3 directSpecular = specular;
        radianceSum +=
            (directDiffuse * kDirectDiffuseBoost + directSpecular * kDirectSpecularBoost) *
            pointRadiance *
            NdotL;
    }

    const float NdotV = clamp(dot(N, V), 0.0f, 1.0f);
    const float3 Fambient = FresnelSchlickRoughness(NdotV, F0, clampedRoughness);
    const float3 kSambient = Fambient;
    const float3 kDambient = (1.0f.xxx - kSambient) * (1.0f - clampedMetalness);

    const float3 irradiance = irradianceMap.SampleLevel(linearClampSampler, N, 0.0f).rgb;
    const float3 ambientDiffuse = irradiance * albedoColor;
    const float3 ambient = kDambient * ambientDiffuse * max(globalParameters.y, 0.0f);

    const float exposure = 1.2f;
    const float3 finalColor = (displayMode == 4) ? radianceSum : (ambient + radianceSum);
    const float3 toneMappedColor = ToneMapReinhard(max(finalColor * exposure, 0.0f.xxx));
    const float3 gammaCorrectedColor = pow(clamp(toneMappedColor, 0.0f.xxx, 1.0f.xxx), 1.0f / 2.2f);
    return float4(gammaCorrectedColor, 1.0f);
}
