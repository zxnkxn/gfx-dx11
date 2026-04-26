#pragma once

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <wrl/client.h>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "gltf_loader.h"

// Include the DirectX libraries
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "windowscodecs.lib")

class Renderer
{
public:
    Renderer();
    ~Renderer();

    // Initialize the DirectX application
    HRESULT Initialize(HINSTANCE hInstance, int nCmdShow);
    const std::wstring& GetInitializationErrorMessage() const;

    // Render a frame
    void Render();

    // Cleanup resources
    void Cleanup();

private:
    enum class DisplayMode : int
    {
        Pbr = 0,
        NormalDistribution = 1,
        Geometry = 2,
        Fresnel = 3,
        DirectLighting = 4,
    };

    struct PointLight
    {
        DirectX::XMFLOAT3 position;
        float radius;
        DirectX::XMFLOAT3 color;
        float intensity;
    };

    struct MeshGeometry
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
        UINT indexCount = 0;
    };

    struct ModelMaterial
    {
        DirectX::XMFLOAT4 baseColorFactor;
        DirectX::XMFLOAT4 emissiveFactor;
        float roughnessFactor;
        float metallicFactor;
        float occlusionStrength;
        float alphaCutoff;
        bool doubleSided;
        Gltf::AlphaMode alphaMode = Gltf::AlphaMode::Opaque;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> baseColorTextureShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> metallicRoughnessTextureShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> emissiveTextureShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> occlusionTextureShaderResourceView;
    };

    struct ModelPrimitive
    {
        MeshGeometry geometry;
        size_t materialIndex = 0;
    };

    struct ModelDrawItem
    {
        size_t primitiveIndex = 0;
        DirectX::XMFLOAT4X4 world;
    };

    struct PointLightGpu
    {
        DirectX::XMFLOAT4 positionRadius;
        DirectX::XMFLOAT4 colorIntensity;
    };

    struct SceneFrameConstants
    {
        DirectX::XMFLOAT4X4 viewProjection;
        DirectX::XMFLOAT4 cameraPosition;
        PointLightGpu pointLights[3];
        DirectX::XMFLOAT4 globalParameters;
    };

    struct SceneObjectConstants
    {
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT4X4 normalMatrix;
        DirectX::XMFLOAT4 baseColorFactor;
        DirectX::XMFLOAT4 emissiveFactor;
        DirectX::XMFLOAT4 materialParameters;
        DirectX::XMFLOAT4 materialFlags;
    };

    struct SkyFrameConstants
    {
        DirectX::XMFLOAT4X4 viewProjection;
        DirectX::XMFLOAT4 cameraPosition;
    };

    struct CaptureConstants
    {
        DirectX::XMFLOAT4 faceForward;
        DirectX::XMFLOAT4 faceRight;
        DirectX::XMFLOAT4 faceUp;
        DirectX::XMFLOAT4 prefilterParameters;
    };

private:
    // Window and application setup
    HRESULT RegisterWindowClass(HINSTANCE hInstance);
    HRESULT CreateAppWindow(HINSTANCE hInstance, int nCmdShow);
    HRESULT InitializeDirectX();
    HRESULT CreateDeviceAndContext();
    HRESULT CreateSwapChain();
    HRESULT CreatePipelineStates();
    HRESULT CreateSamplers();
    HRESULT CreateConstantBuffers();
    HRESULT CreateShaders();
    HRESULT CreateGeometry();
    HRESULT CreateSphereMeshGeometry(UINT latitudeSegments, UINT longitudeSegments, MeshGeometry& geometry, const char* debugName);
    HRESULT LoadSceneModel();
    HRESULT CreateSolidColorTexture(
        const DirectX::XMFLOAT4& color,
        DXGI_FORMAT shaderResourceFormat,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& shaderResourceView,
        const char* debugNamePrefix);
    HRESULT CreateTextureFromEncodedImage(
        const Gltf::Image& image,
        DXGI_FORMAT shaderResourceFormat,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& shaderResourceView,
        const char* debugNamePrefix);
    HRESULT CreateEnvironmentCubemap();
    HRESULT CreateHdriTexture(const struct HdriImage& image);
    HRESULT CreateFloatCubemap(
        UINT faceSize,
        UINT mipLevels,
        bool generateMips,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& shaderResourceView,
        const char* debugNamePrefix);
    HRESULT RenderCubemapFaces(
        ID3D11Texture2D* targetTexture,
        UINT faceSize,
        UINT mipLevel,
        float roughness,
        float sourceCubemapFaceSize,
        ID3D11PixelShader* pixelShader,
        ID3D11ShaderResourceView* sourceShaderResourceView,
        const wchar_t* eventName);
    HRESULT CreateIrradianceMap();
    HRESULT CreatePrefilteredEnvironmentMap();
    HRESULT CreateBrdfIntegrationMap();
    HRESULT CreateFloatTexture2D(
        UINT width,
        UINT height,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& shaderResourceView,
        const char* debugNamePrefix);
    HRESULT RenderFullscreenTexture(
        ID3D11Texture2D* targetTexture,
        UINT width,
        UINT height,
        ID3D11PixelShader* pixelShader,
        const wchar_t* eventName);
    void InitializeLights();
    void SetInitializationError(std::wstring message);
    void UpdateWindowTitle();
    void ReleaseModelResources();

    // Window-size dependent resources
    HRESULT CreateWindowSizeResources();
    HRESULT CreateRenderTargetView();
    HRESULT CreateDepthStencilBuffer();
    void ReleaseWindowSizeResources();
    void ResizeSwapChain(UINT width, UINT height);

    // Camera and rendering
    void ResetCamera();
    void UpdateCamera();
    void RenderEnvironment();
    void RenderModel();
    void DrawModelDrawItem(const ModelDrawItem& object);
    void DrawMesh(
        const MeshGeometry& geometry,
        const DirectX::XMFLOAT4X4& world,
        const ModelMaterial& material,
        float emissiveMultiplier = 1.0f);

    // Shader and resource helpers
    HRESULT CompileShader(const std::wstring& shaderFile, const std::string& entryPoint,
        const std::string& shaderModel, ID3DBlob** blobOut);
    HRESULT LoadShaderBlob(const wchar_t* compiledShaderName, const wchar_t* sourceRelativePath,
        const char* entryPoint, const char* shaderModel, ID3DBlob** blobOut);
    HRESULT CreateMeshGeometry(const void* vertexData, UINT vertexStride, UINT vertexCount,
        const UINT* indexData, UINT indexCount, MeshGeometry& geometry, const char* debugName);
    void SetViewport(UINT width, UINT height) const;
    void BeginEvent(const wchar_t* name) const;
    void EndEvent() const;
    std::filesystem::path FindHdriFile() const;
    std::filesystem::path FindSceneFile() const;

    // Window procedure
    LRESULT HandleWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    // Window properties
    HWND m_hwnd;
    HINSTANCE m_hInstance;
    UINT m_width;
    UINT m_height;
    bool m_isMinimized;
    std::wstring m_title;
    std::wstring m_initializationErrorMessage;
    bool m_comInitialized;

    // Input and camera
    bool m_isOrbiting;
    POINT m_lastMousePosition;
    DirectX::XMFLOAT3 m_cameraTarget;
    float m_cameraDistance;
    float m_cameraYaw;
    float m_cameraPitch;
    DirectX::XMFLOAT3 m_cameraPosition;
    DirectX::XMFLOAT4X4 m_viewMatrix;
    DirectX::XMFLOAT4X4 m_projectionMatrix;

    // Scene state
    std::vector<ModelMaterial> m_modelMaterials;
    std::vector<ModelPrimitive> m_modelPrimitives;
    std::vector<ModelDrawItem> m_modelDrawItems;
    std::array<PointLight, 3> m_pointLights;
    DisplayMode m_displayMode;
    bool m_pointLightsEnabled;
    std::wstring m_loadedHdriFileName;
    std::wstring m_loadedSceneFileName;
    DirectX::XMFLOAT3 m_sceneBoundsMin;
    DirectX::XMFLOAT3 m_sceneBoundsMax;

    // DirectX core objects
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_deviceContext;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthStencilBuffer;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depthStencilView;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizerState;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_doubleSidedRasterizerState;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_skyRasterizerState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthStencilState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_transparentDepthStencilState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_skyDepthStencilState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_alphaBlendState;
    Microsoft::WRL::ComPtr<ID3DUserDefinedAnnotation> m_annotation;

    // Geometry
    MeshGeometry m_sphereGeometry;
    MeshGeometry m_environmentSphereGeometry;

    // Scene pipeline
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_pbrSceneVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pbrScenePixelShader;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_skyVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_skyPixelShader;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_captureVertexShader;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_fullscreenVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_equirectangularToCubemapPixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_irradianceConvolutionPixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_prefilterEnvironmentPixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_brdfIntegrationPixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_sceneInputLayout;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_linearClampSampler;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_linearWrapSampler;

    // Constant buffers
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_sceneFrameConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_sceneObjectConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_skyFrameConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_captureConstantBuffer;

    // Environment resources
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_hdriTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_hdriTextureShaderResourceView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_environmentCubemap;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_environmentCubemapShaderResourceView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_irradianceCubemap;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_irradianceCubemapShaderResourceView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_prefilteredEnvironmentCubemap;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_prefilteredEnvironmentCubemapShaderResourceView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_brdfIntegrationTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_brdfIntegrationShaderResourceView;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_defaultWhiteLinearShaderResourceView;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_defaultWhiteSrgbShaderResourceView;

    // Debug layer state
    bool m_debugLayerEnabled;
};
