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
    float4 baseColorFactor;
    float4 emissiveFactor;
    float4 materialParameters;
    float4 materialFlags;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texCoord : TEXCOORD2;
};

PSInput VS(VSInput input)
{
    PSInput output = (PSInput)0;

    const float4 worldPosition = mul(float4(input.position, 1.0f), world);
    output.position = mul(worldPosition, viewProjection);
    output.worldPosition = worldPosition.xyz;
    output.normal = normalize(mul(float4(input.normal, 0.0f), normalMatrix).xyz);
    output.texCoord = input.texCoord;

    return output;
}
