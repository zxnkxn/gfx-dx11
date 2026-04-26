cbuffer SceneFrameConstants : register(b0)
{
    row_major float4x4 viewProjection;
    float4 cameraPosition;
    float4 cameraForward;
    float4 lightDirectionIntensity;
    float4 lightColorAmbient;
    float4 cascadeSplits;
    float4 shadowTexelData;
    float4 ambientOcclusionData;
    row_major float4x4 shadowMatrices[4];
};

cbuffer SceneObjectConstants : register(b1)
{
    row_major float4x4 world;
    row_major float4x4 normalMatrix;
    float4 albedo;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 albedo : TEXCOORD2;
    float viewDepth : TEXCOORD3;
};

PSInput VS(VSInput input)
{
    PSInput output = (PSInput)0;

    const float4 worldPosition = mul(float4(input.position, 1.0f), world);
    output.position = mul(worldPosition, viewProjection);
    output.worldPosition = worldPosition.xyz;
    output.normal = normalize(mul(float4(input.normal, 0.0f), normalMatrix).xyz);
    output.albedo = albedo.rgb;
    output.viewDepth = dot(worldPosition.xyz - cameraPosition.xyz, cameraForward.xyz);

    return output;
}
