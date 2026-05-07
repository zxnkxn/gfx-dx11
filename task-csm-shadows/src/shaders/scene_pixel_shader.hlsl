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
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 albedo : TEXCOORD2;
};

float3 EvaluatePointLight(PSInput input, PointLightData light, float3 viewDirection)
{
    const float3 toLight = light.positionRadius.xyz - input.worldPosition;
    const float distanceSquared = dot(toLight, toLight);
    const float distanceToLight = sqrt(max(distanceSquared, 1.0e-4f));
    const float radius = light.positionRadius.w;

    const float attenuationBase = saturate(1.0f - distanceToLight / radius);
    const float attenuation = attenuationBase * attenuationBase / max(distanceSquared, 0.25f);
    const float3 lightDirection = toLight / distanceToLight;

    const float3 normal = normalize(input.normal);
    const float diffuse = saturate(dot(normal, lightDirection));
    const float3 halfVector = normalize(lightDirection + viewDirection);
    const float specular = pow(saturate(dot(normal, halfVector)), 48.0f);

    const float3 radiance = light.colorIntensity.rgb * light.colorIntensity.w;
    return radiance * attenuation * (input.albedo * diffuse + specular * 0.18f);
}

float4 PS(PSInput input) : SV_Target
{
    const float3 viewDirection = normalize(cameraPosition.xyz - input.worldPosition);
    float3 color = input.albedo * globalParameters.x;

    [unroll]
    for (int lightIndex = 0; lightIndex < 3; ++lightIndex)
    {
        color += EvaluatePointLight(input, pointLights[lightIndex], viewDirection);
    }

    return float4(color, 1.0f);
}
