cbuffer ShadowFrameConstants : register(b0)
{
    row_major float4x4 viewProjection;
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

float4 VS(VSInput input) : SV_POSITION
{
    const float4 worldPosition = mul(float4(input.position, 1.0f), world);
    return mul(worldPosition, viewProjection);
}
