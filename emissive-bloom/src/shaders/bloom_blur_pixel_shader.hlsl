cbuffer PostProcessConstants : register(b0)
{
    float2 inverseTextureSize;
    float2 blurDirection;
    float4 parameters;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

Texture2D<float4> sourceTexture : register(t0);
SamplerState linearClampSampler : register(s0);

float4 PS(PSInput input) : SV_Target
{
    static const float weight1D[5] =
    {
        0.447892f,
        0.238827f,
        0.035753f,
        0.001459f,
        0.000016f,
    };

    const float blurScale = max(parameters.x, 1.0f);
    const float2 texelOffset = inverseTextureSize * blurDirection * blurScale;
    float4 result = weight1D[0] * sourceTexture.Sample(linearClampSampler, input.texCoord);

    [unroll]
    for (int sampleIndex = 1; sampleIndex <= 4; ++sampleIndex)
    {
        const float2 offset = texelOffset * sampleIndex;
        result += weight1D[sampleIndex] * sourceTexture.Sample(linearClampSampler, input.texCoord + offset);
        result += weight1D[sampleIndex] * sourceTexture.Sample(linearClampSampler, input.texCoord - offset);
    }

    return result;
}
