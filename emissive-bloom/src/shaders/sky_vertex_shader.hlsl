cbuffer SkyFrameConstants : register(b0)
{
    row_major float4x4 viewProjection;
    float4 cameraPosition;
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
};

PSInput VS(VSInput input)
{
    PSInput output = (PSInput)0;

    const float3 worldPosition = input.position * 80.0f + cameraPosition.xyz;
    output.worldPosition = worldPosition;
    output.position = mul(float4(worldPosition, 1.0f), viewProjection);

    return output;
}
