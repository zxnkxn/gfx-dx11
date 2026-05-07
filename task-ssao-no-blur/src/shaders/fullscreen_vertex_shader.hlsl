struct VSOutput
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

VSOutput VS(uint vertexId : SV_VertexID)
{
    static const float2 positions[3] =
    {
        float2(-1.0f, -1.0f),
        float2(-1.0f, 3.0f),
        float2(3.0f, -1.0f),
    };

    static const float2 texCoords[3] =
    {
        float2(0.0f, 1.0f),
        float2(0.0f, -1.0f),
        float2(2.0f, 1.0f),
    };

    VSOutput output = (VSOutput)0;
    output.position = float4(positions[vertexId], 0.0f, 1.0f);
    output.texCoord = texCoords[vertexId];
    return output;
}
