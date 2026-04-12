#pragma once

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <string>

// Include the DirectX libraries
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

using namespace DirectX;

// Structure for constant buffer
struct ConstantBuffer
{
    XMMATRIX world;
    XMMATRIX view;
    XMMATRIX projection;
};

class Renderer
{
public:
    Renderer();
    ~Renderer();

    // Initialize the DirectX application
    HRESULT Initialize(HINSTANCE hInstance, int nCmdShow);

    // Handle window messages
    void HandleMessages();

    // Render a frame
    void Render();

    // Cleanup resources
    void Cleanup();

private:
    // Register window class
    HRESULT RegisterWindowClass(HINSTANCE hInstance);

    // Create window
    HRESULT CreateAppWindow(HINSTANCE hInstance);

    // Initialize DirectX
    HRESULT InitializeDirectX();

    // Create swap chain
    HRESULT CreateSwapChain();

    // Create render target view
    HRESULT CreateRenderTargetView();

    // Create depth stencil buffer
    HRESULT CreateDepthStencilBuffer();

    // Create viewport
    void CreateViewport();

    // Compile shader
    HRESULT CompileShader(const std::wstring& shaderFile, const std::string& entryPoint,
        const std::string& shaderModel, ID3DBlob** blobOut);

    // Load a precompiled shader blob and fall back to source compilation if needed
    HRESULT LoadShaderBlob(const wchar_t* compiledShaderName, const wchar_t* sourceRelativePath,
        const char* entryPoint, const char* shaderModel, ID3DBlob** blobOut);

    // Create shaders
    HRESULT CreateShaders();

    // Create constant buffer
    HRESULT CreateConstantBuffer();

    // Create cube geometry
    HRESULT CreateCubeGeometry();

    // Update constant buffer
    void UpdateConstantBuffer();

    // Resize swap chain
    void ResizeSwapChain(UINT width, UINT height);

    // RenderDoc markers
    void BeginEvent(const wchar_t* name) const;
    void EndEvent() const;

    // Window procedure
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    // Window properties
    HWND m_hwnd;
    HINSTANCE m_hInstance;
    UINT m_width;
    UINT m_height;
    std::wstring m_title;

    // DirectX properties
    ID3D11Device* m_device;
    ID3D11DeviceContext* m_deviceContext;
    IDXGISwapChain* m_swapChain;
    ID3D11RenderTargetView* m_renderTargetView;
    ID3D11Texture2D* m_depthStencilBuffer;
    ID3D11DepthStencilView* m_depthStencilView;
    ID3D11RasterizerState* m_rasterizerState;
    ID3D11DepthStencilState* m_depthStencilState;
    ID3DUserDefinedAnnotation* m_annotation;

    // Shaders
    ID3D11VertexShader* m_vertexShader;
    ID3D11PixelShader* m_pixelShader;
    ID3D11InputLayout* m_inputLayout;

    // Constant buffer
    ID3D11Buffer* m_constantBuffer;

    // Cube geometry
    ID3D11Buffer* m_vertexBuffer;
    ID3D11Buffer* m_indexBuffer;
    UINT m_indexCount;

    // Transformation matrices
    XMMATRIX m_worldMatrix;
    XMMATRIX m_viewMatrix;
    XMMATRIX m_projectionMatrix;

    // Camera properties
    XMVECTOR m_cameraPosition;
    XMVECTOR m_cameraTarget;
    XMVECTOR m_cameraUp;

    // Rotation angle
    float m_rotation;

    // Debug layer enabled flag
    bool m_debugLayerEnabled;
};
