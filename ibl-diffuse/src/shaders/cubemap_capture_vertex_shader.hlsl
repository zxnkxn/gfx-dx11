cbuffer CaptureConstants : register(b0)
{
    float4 faceForward;
    float4 faceRight;
    float4 faceUp;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 direction : TEXCOORD0;
};

VSOutput VS(uint vertexId : SV_VertexID)
{
    static const float2 clipSpaceVertices[6] =
    {
        float2(-1.0f, -1.0f),
        float2(-1.0f,  1.0f),
        float2( 1.0f,  1.0f),
        float2(-1.0f, -1.0f),
        float2( 1.0f,  1.0f),
        float2( 1.0f, -1.0f),
    };

    VSOutput output = (VSOutput)0;
    const float2 clipPosition = clipSpaceVertices[vertexId];

    output.position = float4(clipPosition, 0.0f, 1.0f);

    // Cubemap face Y must follow the face-up basis directly; inverting it
    // flips the side faces upside down after the equirectangular conversion.
    output.direction =
        faceForward.xyz +
        clipPosition.x * faceRight.xyz +
        clipPosition.y * faceUp.xyz;

    return output;
}
