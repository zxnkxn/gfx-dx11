cbuffer NormalPrepassConstants : register(b0)
{
    row_major float4x4 viewMatrix;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 albedo : TEXCOORD2;
    float viewDepth : TEXCOORD3;
};

float4 PS(PSInput input) : SV_Target
{
    const float3 viewNormal = normalize(mul(float4(normalize(input.normal), 0.0f), viewMatrix).xyz);
    return float4(viewNormal * 0.5f + 0.5f, 1.0f);
}
