// Vertex shader input structure
struct VS_INPUT
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
};

// Pixel shader input structure
struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float3 normal : NORMAL;
};

// Constant buffer for transformation matrices
cbuffer ConstantBuffer : register(b0)
{
    matrix world;
    matrix view;
    matrix projection;
};

// Vertex shader
PS_INPUT VS(VS_INPUT input)
{
    PS_INPUT output = (PS_INPUT) 0;
    
    // Transform vertex position to world space
    float4 worldPosition = mul(float4(input.pos, 1.0f), world);
    
    // Transform vertex position to view space
    float4 viewPosition = mul(worldPosition, view);
    
    // Transform vertex position to projection space
    output.pos = mul(viewPosition, projection);
    
    // Transform normal to world space
    output.normal = normalize(mul(input.normal, (float3x3) world));
    
    return output;
}