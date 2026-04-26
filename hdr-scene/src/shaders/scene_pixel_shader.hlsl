cbuffer SceneFrameConstants : register(b0)
{
    row_major float4x4 viewProjection;
    float4 cameraPosition;
    float4 cameraForward;
    float4 lightDirectionIntensity;
    float4 lightColorAmbient;
    float4 cascadeSplits;
    float4 shadowTexelData;
    row_major float4x4 shadowMatrices[4];
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
    float viewDepth : TEXCOORD3;
};

Texture2DArray<float> shadowMapTexture : register(t0);
SamplerComparisonState shadowSampler : register(s0);

float SampleShadowCascade(int cascadeIndex, float3 worldPosition, float3 worldNormal)
{
    const float4 shadowPosition = mul(float4(worldPosition, 1.0f), shadowMatrices[cascadeIndex]);
    const float2 uv = shadowPosition.xy;

    const float texelSize = shadowTexelData.x;
    const float geometricBias = texelSize;
    const float lightAlignment = saturate(dot(normalize(worldNormal), -lightDirectionIntensity.xyz));
    const float grazingAngle = 1.0f - lightAlignment;
    const float cascadeScale = 1.0f + 0.35f * (float)cascadeIndex;
    const float normalBias =
        shadowTexelData.y *
        cascadeScale *
        grazingAngle *
        grazingAngle;
    const float compareDepth = shadowPosition.z - geometricBias - normalBias;

    float visibility = 0.0f;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 sampleUv = uv + float2((float)x, (float)y) * texelSize;
            visibility += shadowMapTexture.SampleCmpLevelZero(
                shadowSampler,
                float3(sampleUv, (float)cascadeIndex),
                compareDepth);
        }
    }

    return visibility / 9.0f;
}

float ComputeShadowVisibility(float3 worldPosition, float3 worldNormal, float viewDepth)
{
    if (viewDepth <= 0.0f || viewDepth > cascadeSplits.w)
    {
        return 1.0f;
    }

    int cascadeIndex = 3;
    if (viewDepth < cascadeSplits.x)
    {
        cascadeIndex = 0;
    }
    else if (viewDepth < cascadeSplits.y)
    {
        cascadeIndex = 1;
    }
    else if (viewDepth < cascadeSplits.z)
    {
        cascadeIndex = 2;
    }

    return SampleShadowCascade(cascadeIndex, worldPosition, worldNormal);
}

float4 PS(PSInput input) : SV_Target
{
    const float3 normal = normalize(input.normal);
    const float3 viewDirection = normalize(cameraPosition.xyz - input.worldPosition);
    const float3 lightDirection = normalize(-lightDirectionIntensity.xyz);
    const float3 lightColor = lightColorAmbient.rgb * lightDirectionIntensity.w;

    const float shadowVisibility =
        (albedo.a > 0.5f && dot(normal, lightDirection) > 0.0f)
        ? ComputeShadowVisibility(input.worldPosition, normal, input.viewDepth)
        : 1.0f;

    const float diffuseTerm = saturate(dot(normal, lightDirection));
    const float3 halfVector = normalize(lightDirection + viewDirection);
    const float specularTerm = pow(saturate(dot(normal, halfVector)), 72.0f) * shadowVisibility;

    const float3 ambient = input.albedo * lightColorAmbient.w;
    const float3 diffuse = input.albedo * lightColor * diffuseTerm * shadowVisibility;
    const float3 specular = lightColor * specularTerm * 0.18f;

    return float4(ambient + diffuse + specular, 1.0f);
}
