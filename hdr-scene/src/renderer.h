#pragma once

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <string>
#include <vector>

// Include the DirectX libraries
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

class Renderer
{
public:
    Renderer();
    ~Renderer();

    // Initialize the DirectX application
    HRESULT Initialize(HINSTANCE hInstance, int nCmdShow);

    // Render a frame
    void Render();

    // Cleanup resources
    void Cleanup();

private:
    static constexpr UINT kShadowCascadeCount = 4;
    static constexpr UINT kSsaoSampleCount = 32;
    static constexpr UINT kSsaoNoiseDimension = 4;

    enum class MeshKind
    {
        Cube,
        Plane,
    };

    struct DirectionalLight
    {
        DirectX::XMFLOAT3 direction;
        float intensity;
        DirectX::XMFLOAT3 color;
        float ambientIntensity;
    };

    struct SceneObject
    {
        MeshKind meshKind;
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT3 albedo;
        float shadowReceiver = 1.0f;
    };

    struct MeshGeometry
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
        UINT indexCount = 0;
    };

    struct RenderTexture
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shaderResourceView;
        UINT width = 0;
        UINT height = 0;
    };

    struct SceneFrameConstants
    {
        DirectX::XMFLOAT4X4 viewProjection;
        DirectX::XMFLOAT4 cameraPosition;
        DirectX::XMFLOAT4 cameraForward;
        DirectX::XMFLOAT4 lightDirectionIntensity;
        DirectX::XMFLOAT4 lightColorAmbient;
        DirectX::XMFLOAT4 cascadeSplits;
        DirectX::XMFLOAT4 shadowTexelData;
        DirectX::XMFLOAT4 ambientOcclusionData;
        DirectX::XMFLOAT4X4 shadowMatrices[kShadowCascadeCount];
    };

    struct SceneObjectConstants
    {
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT4X4 normalMatrix;
        DirectX::XMFLOAT4 albedo;
    };

    struct ShadowFrameConstants
    {
        DirectX::XMFLOAT4X4 viewProjection;
    };

    struct NormalPrepassConstants
    {
        DirectX::XMFLOAT4X4 viewMatrix;
    };

    struct SsaoConstants
    {
        DirectX::XMFLOAT4X4 projection;
        DirectX::XMFLOAT4X4 inverseProjection;
        DirectX::XMFLOAT4 ssaoParameters;
        DirectX::XMFLOAT4 inverseTextureSize;
        DirectX::XMFLOAT4 samples[kSsaoSampleCount];
        DirectX::XMFLOAT4 noise[kSsaoNoiseDimension * kSsaoNoiseDimension];
    };

    struct DownsampleConstants
    {
        DirectX::XMFLOAT2 sourceTexelSize;
        DirectX::XMFLOAT2 padding;
    };

    struct AdaptationConstants
    {
        float deltaTime;
        float adaptationRate;
        float minLuminance;
        float padding;
    };

    struct ToneMappingConstants
    {
        float middleGray;
        float whitePoint;
        float minLuminance;
        float padding;
    };

    struct ShadowCascade
    {
        DirectX::XMFLOAT4X4 viewProjection = {};
        DirectX::XMFLOAT4X4 worldToShadowTexture = {};
        float splitDistance = 0.0f;
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
    HRESULT CreateShadowResources();
    void CreateSceneObjects();
    void InitializeLighting();
    void UpdateSceneBounds();
    void UpdateWindowTitle();

    // Window-size dependent resources
    HRESULT CreateWindowSizeResources();
    HRESULT CreateRenderTargetView();
    HRESULT CreateDepthStencilBuffer();
    HRESULT CreateHdrSceneBuffer();
    HRESULT CreateNormalBuffer();
    HRESULT CreateAmbientOcclusionBuffer();
    HRESULT CreateLuminanceResources();
    void ReleaseWindowSizeResources();
    void ResizeSwapChain(UINT width, UINT height);

    // Rendering
    void InitializeSsaoKernel();
    void Update(float deltaTime);
    void UpdateCamera();
    void UpdateShadowCascades();
    void RenderShadowMaps();
    void RenderNormalDepthPrepass();
    void RenderAmbientOcclusion();
    void RenderSceneToHdr();
    void DrawSceneObject(const SceneObject& object);
    void DrawSceneObjectShadow(const SceneObject& object);
    void ComputeAverageLuminance();
    void AdaptEye(float deltaTime);
    void ToneMapToBackBuffer();

    // Shader and resource helpers
    HRESULT CompileShader(const std::wstring& shaderFile, const std::string& entryPoint,
        const std::string& shaderModel, ID3DBlob** blobOut);
    HRESULT LoadShaderBlob(const wchar_t* compiledShaderName, const wchar_t* sourceRelativePath,
        const char* entryPoint, const char* shaderModel, ID3DBlob** blobOut);
    HRESULT CreateRenderTexture(UINT width, UINT height, DXGI_FORMAT format, RenderTexture& texture,
        const char* debugName);
    HRESULT CreateMeshGeometry(const void* vertexData, UINT vertexStride, UINT vertexCount,
        const UINT* indexData, UINT indexCount, MeshGeometry& geometry, const char* debugName);
    void SetViewport(UINT width, UINT height) const;
    void BeginEvent(const wchar_t* name) const;
    void EndEvent() const;

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

    // Timing and input
    std::chrono::steady_clock::time_point m_previousFrameTime;
    std::array<bool, 256> m_keyStates;
    bool m_isOrbiting;
    POINT m_lastMousePosition;

    // Camera
    DirectX::XMFLOAT3 m_cameraTarget;
    float m_cameraDistance;
    float m_cameraYaw;
    float m_cameraPitch;
    DirectX::XMFLOAT3 m_cameraPosition;
    DirectX::XMFLOAT3 m_cameraForward;
    DirectX::XMFLOAT4X4 m_viewMatrix;
    DirectX::XMFLOAT4X4 m_projectionMatrix;

    // Scene state
    std::vector<SceneObject> m_sceneObjects;
    DirectionalLight m_directionalLight;
    DirectX::XMFLOAT3 m_sceneBoundsMin;
    DirectX::XMFLOAT3 m_sceneBoundsMax;
    std::array<ShadowCascade, kShadowCascadeCount> m_shadowCascades;
    bool m_ssaoEnabled;
    bool m_ssaoPreviewEnabled;
    std::array<DirectX::XMFLOAT4, kSsaoSampleCount> m_ssaoSamples;
    std::array<DirectX::XMFLOAT4, kSsaoNoiseDimension * kSsaoNoiseDimension> m_ssaoNoise;

    // DirectX core objects
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_deviceContext;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthStencilBuffer;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depthStencilView;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_depthStencilShaderResourceView;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizerState;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_shadowRasterizerState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthStencilState;
    Microsoft::WRL::ComPtr<ID3DUserDefinedAnnotation> m_annotation;

    // Geometry
    MeshGeometry m_cubeGeometry;
    MeshGeometry m_planeGeometry;

    // Scene pipeline
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_sceneVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_scenePixelShader;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_shadowVertexShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_sceneInputLayout;

    // Post-processing pipeline
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_fullscreenVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_normalPrepassPixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ssaoPixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_luminanceInitialPixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_luminanceReducePixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_luminanceAdaptPixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_toneMappingPixelShader;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_pointClampSampler;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_linearClampSampler;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_shadowComparisonSampler;

    // Constant buffers
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_sceneFrameConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_sceneObjectConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_shadowFrameConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_normalPrepassConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_ssaoConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_downsampleConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_adaptationConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_toneMappingConstantBuffer;

    // Shadow resources
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_shadowMapTexture;
    std::array<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>, kShadowCascadeCount> m_shadowCascadeDepthStencilViews;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_shadowMapShaderResourceView;

    // HDR render targets
    RenderTexture m_hdrSceneTexture;
    RenderTexture m_normalTexture;
    RenderTexture m_ssaoTexture;
    std::vector<RenderTexture> m_luminanceChain;
    std::array<RenderTexture, 2> m_adaptedLuminanceTextures;
    UINT m_currentAdaptedLuminanceIndex;

    // Debug layer state
    bool m_debugLayerEnabled;
};
