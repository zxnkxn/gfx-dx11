#pragma once

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <wrl/client.h>

#include <array>
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
    enum class DisplayMode : int
    {
        Pbr = 0,
        NormalDistribution = 1,
        Geometry = 2,
        Fresnel = 3,
    };

    struct PointLight
    {
        DirectX::XMFLOAT3 position;
        float radius;
        DirectX::XMFLOAT3 color;
        float intensity;
    };

    struct SphereInstance
    {
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT3 albedo;
        float roughness;
        float metalness;
        float emissiveStrength;
    };

    struct MeshGeometry
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
        UINT indexCount = 0;
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
        DirectX::XMFLOAT4 albedo;
        DirectX::XMFLOAT4 materialParameters;
    };

    struct SkyFrameConstants
    {
        DirectX::XMFLOAT4X4 viewProjection;
        DirectX::XMFLOAT4 cameraPosition;
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
    HRESULT CreateEnvironmentCubemap();
    void CreateSceneObjects();
    void InitializeLights();
    void UpdateWindowTitle();

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
    void RenderSpheres();
    void DrawSphereInstance(const SphereInstance& object);

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
    std::vector<SphereInstance> m_sphereInstances;
    std::array<PointLight, 3> m_pointLights;
    DisplayMode m_displayMode;

    // DirectX core objects
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_deviceContext;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthStencilBuffer;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depthStencilView;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizerState;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_skyRasterizerState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthStencilState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_skyDepthStencilState;
    Microsoft::WRL::ComPtr<ID3DUserDefinedAnnotation> m_annotation;

    // Geometry
    MeshGeometry m_sphereGeometry;

    // Scene pipeline
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_pbrSceneVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pbrScenePixelShader;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_skyVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_skyPixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_sceneInputLayout;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_linearClampSampler;

    // Constant buffers
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_sceneFrameConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_sceneObjectConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_skyFrameConstantBuffer;

    // Environment resources
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_environmentCubemap;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_environmentCubemapShaderResourceView;

    // Debug layer state
    bool m_debugLayerEnabled;
};
