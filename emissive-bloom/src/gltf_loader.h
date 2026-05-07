#pragma once

#include <windows.h>
#include <directxmath.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Gltf
{
    enum class AlphaMode : int
    {
        Opaque = 0,
        Mask = 1,
        Blend = 2,
    };

    struct Vertex
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT2 texCoord;
    };

    struct Image
    {
        std::vector<std::uint8_t> encodedBytes;
        std::string mimeType;
        std::wstring debugName;
    };

    struct TextureRef
    {
        int imageIndex = -1;
        int texCoord = 0;
    };

    struct Material
    {
        DirectX::XMFLOAT4 baseColorFactor = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        DirectX::XMFLOAT3 emissiveFactor = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        float metallicFactor = 1.0f;
        float roughnessFactor = 1.0f;
        float occlusionStrength = 1.0f;
        float alphaCutoff = 0.5f;
        bool doubleSided = false;
        AlphaMode alphaMode = AlphaMode::Opaque;
        TextureRef baseColorTexture;
        TextureRef metallicRoughnessTexture;
        TextureRef emissiveTexture;
        TextureRef occlusionTexture;
    };

    struct Primitive
    {
        std::vector<Vertex> vertices;
        std::vector<UINT> indices;
        size_t materialIndex = 0;
    };

    struct NodePrimitive
    {
        size_t primitiveIndex = 0;
        DirectX::XMFLOAT4X4 world = {};
    };

    struct Scene
    {
        std::vector<Image> images;
        std::vector<Material> materials;
        std::vector<Primitive> primitives;
        std::vector<NodePrimitive> nodePrimitives;
        DirectX::XMFLOAT3 boundsMin = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        DirectX::XMFLOAT3 boundsMax = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        std::wstring sourceFileName;
    };

    HRESULT LoadScene(const std::filesystem::path& filePath, Scene& scene);
    std::wstring GetLastErrorMessage();
}
