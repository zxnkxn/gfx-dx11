cbuffer SkyFrameConstants : register(b0)
{
    row_major float4x4 viewProjection;
    float4 cameraPosition;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
};

struct PSOutput
{
    float4 sceneColor : SV_Target0;
    float4 bloomMask : SV_Target1;
};

TextureCube<float4> environmentMap : register(t0);
SamplerState linearClampSampler : register(s0);

PSOutput PS(PSInput input)
{
    PSOutput output = (PSOutput)0;

    const float3 direction = normalize(input.worldPosition - cameraPosition.xyz);
    const float3 environmentColor = environmentMap.SampleLevel(linearClampSampler, direction, 0.0f).rgb;
    output.sceneColor = float4(environmentColor, 1.0f);
    output.bloomMask = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return output;
}
