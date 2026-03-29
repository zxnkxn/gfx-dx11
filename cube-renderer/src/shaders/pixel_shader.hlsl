// Pixel shader input structure
struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float3 normal : NORMAL;
};

// Pixel shader
float4 PS(PS_INPUT input) : SV_Target
{
    // Simple lighting calculation
    float3 lightDir = normalize(float3(0.5f, 1.0f, 0.5f));
    float diffuse = max(0.0f, dot(input.normal, lightDir));
    
    // Simple color based on normal direction
    float3 color = (input.normal + 1.0f) * 0.5f;
    
    return float4(color * (0.3f + 0.7f * diffuse), 1.0f);
}