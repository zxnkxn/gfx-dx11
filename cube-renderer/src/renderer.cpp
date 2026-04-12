#include "renderer.h"

#include <d3d11sdklayers.h>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <vector>

// Helper macro for HRESULT checking with error output
#define DXCall(x) \
    do { \
        const HRESULT dxCallHr = (x); \
        if (FAILED(dxCallHr)) { \
            OutputDebugStringA("DirectX call failed: " #x "\n"); \
            return dxCallHr; \
        } \
    } while(0)

namespace
{
    namespace fs = std::filesystem;

    template <typename T>
    void SafeRelease(T*& resource) noexcept
    {
        if (resource != nullptr)
        {
            resource->Release();
            resource = nullptr;
        }
    }

    void SetDebugName(ID3D11DeviceChild* resource, std::string_view name) noexcept
    {
        if (resource != nullptr && !name.empty())
        {
            resource->SetPrivateData(
                WKPDID_D3DDebugObjectName,
                static_cast<UINT>(name.size()),
                name.data());
        }
    }

    void SetDebugName(IDXGIObject* resource, std::string_view name) noexcept
    {
        if (resource != nullptr && !name.empty())
        {
            resource->SetPrivateData(
                WKPDID_D3DDebugObjectName,
                static_cast<UINT>(name.size()),
                name.data());
        }
    }

    fs::path GetExecutableDirectory()
    {
        std::wstring modulePath(MAX_PATH, L'\0');

        while (true)
        {
            const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
            if (length == 0)
            {
                return {};
            }

            if (length < modulePath.size() - 1)
            {
                modulePath.resize(length);
                return fs::path(modulePath).parent_path();
            }

            modulePath.resize(modulePath.size() * 2);
        }
    }

    bool FileExists(const fs::path& path)
    {
        std::error_code errorCode;
        return fs::is_regular_file(path, errorCode);
    }
}

// Vertex structure
struct Vertex
{
    XMFLOAT3 position;
    XMFLOAT3 normal;
};

Renderer::Renderer() :
    m_hwnd(nullptr),
    m_hInstance(nullptr),
    m_width(800),
    m_height(600),
    m_title(L"DirectX 11 Cube"),
    m_device(nullptr),
    m_deviceContext(nullptr),
    m_swapChain(nullptr),
    m_renderTargetView(nullptr),
    m_depthStencilBuffer(nullptr),
    m_depthStencilView(nullptr),
    m_rasterizerState(nullptr),
    m_depthStencilState(nullptr),
    m_annotation(nullptr),
    m_vertexShader(nullptr),
    m_pixelShader(nullptr),
    m_inputLayout(nullptr),
    m_constantBuffer(nullptr),
    m_vertexBuffer(nullptr),
    m_indexBuffer(nullptr),
    m_indexCount(0),
    m_rotation(0.0f),
    m_debugLayerEnabled(false)
{
    // Initialize matrices
    m_worldMatrix = XMMatrixIdentity();
    m_viewMatrix = XMMatrixIdentity();
    m_projectionMatrix = XMMatrixIdentity();

    // Initialize camera
    m_cameraPosition = XMVectorSet(0.0f, 1.0f, -5.0f, 0.0f);
    m_cameraTarget = XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f);
    m_cameraUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
}

Renderer::~Renderer()
{
    Cleanup();
}

HRESULT Renderer::Initialize(HINSTANCE hInstance, int nCmdShow)
{
    OutputDebugString(L"Initializing DirectX renderer...\n");

    m_hInstance = hInstance;

    // Register window class
    OutputDebugString(L"Registering window class...\n");
    HRESULT hr = RegisterWindowClass(hInstance);
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to register window class\n");
        return hr;
    }

    // Create window
    OutputDebugString(L"Creating application window...\n");
    hr = CreateAppWindow(hInstance);
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to create application window\n");
        return hr;
    }

    // Initialize DirectX
    OutputDebugString(L"Initializing DirectX...\n");
    hr = InitializeDirectX();
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to initialize DirectX\n");
        return hr;
    }

    // Create shaders
    OutputDebugString(L"Creating shaders...\n");
    hr = CreateShaders();
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to create shaders\n");
        return hr;
    }

    // Create constant buffer
    OutputDebugString(L"Creating constant buffer...\n");
    hr = CreateConstantBuffer();
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to create constant buffer\n");
        return hr;
    }

    // Create cube geometry
    OutputDebugString(L"Creating cube geometry...\n");
    hr = CreateCubeGeometry();
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to create cube geometry\n");
        return hr;
    }

    // Show window
    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);

    OutputDebugString(L"DirectX renderer initialized successfully\n");
    return S_OK;
}

HRESULT Renderer::RegisterWindowClass(HINSTANCE hInstance)
{
    WNDCLASSEX wcex = { 0 };
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WindowProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = L"DirectX11CubeClass";

    if (!RegisterClassEx(&wcex))
    {
        const DWORD error = GetLastError();
        if (error == ERROR_CLASS_ALREADY_EXISTS)
        {
            return S_OK;
        }

        OutputDebugString((L"Failed to register window class. Error: " + std::to_wstring(error) + L"\n").c_str());
        return HRESULT_FROM_WIN32(error);
    }

    return S_OK;
}

HRESULT Renderer::CreateAppWindow(HINSTANCE hInstance)
{
    RECT rc = { 0, 0, (LONG)m_width, (LONG)m_height };
    if (!AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE))
    {
        const DWORD error = GetLastError();
        OutputDebugString((L"Failed to adjust window rect. Error: " + std::to_wstring(error) + L"\n").c_str());
        return HRESULT_FROM_WIN32(error);
    }

    m_hwnd = CreateWindow(
        L"DirectX11CubeClass",
        m_title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rc.right - rc.left,
        rc.bottom - rc.top,
        nullptr,
        nullptr,
        hInstance,
        nullptr);

    if (!m_hwnd)
    {
        const DWORD error = GetLastError();
        OutputDebugString((L"Failed to create window. Error: " + std::to_wstring(error) + L"\n").c_str());
        return HRESULT_FROM_WIN32(error);
    }

    // Set window user data to this instance
    SetLastError(ERROR_SUCCESS);
    SetWindowLongPtr(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    const DWORD error = GetLastError();
    if (error != ERROR_SUCCESS)
    {
        OutputDebugString((L"Failed to set window user data. Error: " + std::to_wstring(error) + L"\n").c_str());
        return HRESULT_FROM_WIN32(error);
    }

    return S_OK;
}

HRESULT Renderer::InitializeDirectX()
{
    // Check if we're in debug mode
    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    m_debugLayerEnabled = true;
    OutputDebugString(L"Debug layer requested\n");
#endif

    // Driver types
    D3D_DRIVER_TYPE driverTypes[] =
    {
        D3D_DRIVER_TYPE_HARDWARE,
        D3D_DRIVER_TYPE_WARP,
        D3D_DRIVER_TYPE_REFERENCE,
    };
    UINT numDriverTypes = ARRAYSIZE(driverTypes);

    // Feature levels
    D3D_FEATURE_LEVEL featureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    UINT numFeatureLevels = ARRAYSIZE(featureLevels);

    HRESULT hr = S_OK;

    // Try to create device with different driver types
    for (UINT driverTypeIndex = 0; driverTypeIndex < numDriverTypes; driverTypeIndex++)
    {
        const D3D_DRIVER_TYPE driverType = driverTypes[driverTypeIndex];
        OutputDebugString((L"Trying to create device with driver type: " + std::to_wstring(driverTypeIndex) + L"\n").c_str());

        hr = D3D11CreateDevice(
            nullptr,
            driverType,
            nullptr,
            createDeviceFlags,
            featureLevels,
            numFeatureLevels,
            D3D11_SDK_VERSION,
            &m_device,
            nullptr,
            &m_deviceContext);

#ifdef _DEBUG
        if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING && (createDeviceFlags & D3D11_CREATE_DEVICE_DEBUG) != 0)
        {
            OutputDebugString(L"DirectX debug layer is not installed. Retrying without it.\n");
            m_debugLayerEnabled = false;
            SafeRelease(m_device);
            SafeRelease(m_deviceContext);

            hr = D3D11CreateDevice(
                nullptr,
                driverType,
                nullptr,
                createDeviceFlags & ~D3D11_CREATE_DEVICE_DEBUG,
                featureLevels,
                numFeatureLevels,
                D3D11_SDK_VERSION,
                &m_device,
                nullptr,
                &m_deviceContext);
        }
#endif

        if (SUCCEEDED(hr))
        {
            OutputDebugString((L"Device created successfully with driver type: " + std::to_wstring(driverTypeIndex) + L"\n").c_str());
            break;
        }
        else
        {
            OutputDebugString((L"Failed to create device with driver type: " + std::to_wstring(driverTypeIndex) + L"\n").c_str());
        }
    }

    if (FAILED(hr))
    {
        OutputDebugString(L"Failed to create device with any driver type\n");
        return hr;
    }

#ifdef _DEBUG
    if (m_debugLayerEnabled)
    {
        ID3D11InfoQueue* infoQueue = nullptr;
        if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&infoQueue))))
        {
            infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
            SafeRelease(infoQueue);
        }
    }
#endif

    if (FAILED(m_deviceContext->QueryInterface(IID_PPV_ARGS(&m_annotation))))
    {
        m_annotation = nullptr;
        OutputDebugString(L"ID3DUserDefinedAnnotation is not available on this device context.\n");
    }

    // Create swap chain
    OutputDebugString(L"Creating swap chain...\n");
    hr = CreateSwapChain();
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to create swap chain\n");
        return hr;
    }

    // Create render target view
    OutputDebugString(L"Creating render target view...\n");
    hr = CreateRenderTargetView();
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to create render target view\n");
        return hr;
    }

    // Create depth stencil buffer
    OutputDebugString(L"Creating depth stencil buffer...\n");
    hr = CreateDepthStencilBuffer();
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to create depth stencil buffer\n");
        return hr;
    }

    // Create viewport
    OutputDebugString(L"Creating viewport...\n");
    CreateViewport();

    // Create rasterizer state
    OutputDebugString(L"Creating rasterizer state...\n");
    D3D11_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_BACK;
    rasterDesc.FrontCounterClockwise = FALSE;
    rasterDesc.DepthBias = 0;
    rasterDesc.DepthBiasClamp = 0.0f;
    rasterDesc.SlopeScaledDepthBias = 0.0f;
    rasterDesc.DepthClipEnable = TRUE;
    rasterDesc.ScissorEnable = FALSE;
    rasterDesc.MultisampleEnable = FALSE;
    rasterDesc.AntialiasedLineEnable = FALSE;

    hr = m_device->CreateRasterizerState(&rasterDesc, &m_rasterizerState);
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to create rasterizer state\n");
        return hr;
    }
    SetDebugName(m_rasterizerState, "MainRasterizerState");
    m_deviceContext->RSSetState(m_rasterizerState);

    // Create depth stencil state
    OutputDebugString(L"Creating depth stencil state...\n");
    D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;
    depthStencilDesc.StencilEnable = FALSE;

    hr = m_device->CreateDepthStencilState(&depthStencilDesc, &m_depthStencilState);
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to create depth stencil state\n");
        return hr;
    }
    SetDebugName(m_depthStencilState, "MainDepthStencilState");
    m_deviceContext->OMSetDepthStencilState(m_depthStencilState, 1);

    OutputDebugString(L"DirectX initialized successfully\n");
    return S_OK;
}

HRESULT Renderer::CreateSwapChain()
{
    // Get DXGI factory
    OutputDebugString(L"Getting DXGI factory...\n");
    IDXGIDevice* dxgiDevice = nullptr;
    DXCall(m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice));

    IDXGIAdapter* dxgiAdapter = nullptr;
    DXCall(dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&dxgiAdapter));

    IDXGIFactory* dxgiFactory = nullptr;
    DXCall(dxgiAdapter->GetParent(__uuidof(IDXGIFactory), (void**)&dxgiFactory));

    // Create swap chain
    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = 1;
    swapChainDesc.BufferDesc.Width = m_width;
    swapChainDesc.BufferDesc.Height = m_height;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
    swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = m_hwnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.Windowed = TRUE;

    HRESULT hr = dxgiFactory->CreateSwapChain(m_device, &swapChainDesc, &m_swapChain);

    // Release temporary interfaces
    SafeRelease(dxgiDevice);
    SafeRelease(dxgiAdapter);
    SafeRelease(dxgiFactory);

    if (FAILED(hr)) {
        OutputDebugString((L"Failed to create swap chain. HRESULT: " + std::to_wstring(hr) + L"\n").c_str());
    }
    else
    {
        SetDebugName(m_swapChain, "MainSwapChain");
    }

    return hr;
}

HRESULT Renderer::CreateRenderTargetView()
{
    // Get back buffer
    OutputDebugString(L"Getting back buffer...\n");
    ID3D11Texture2D* backBuffer = nullptr;
    DXCall(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer));

    // Create render target view
    OutputDebugString(L"Creating render target view...\n");
    HRESULT hr = m_device->CreateRenderTargetView(backBuffer, nullptr, &m_renderTargetView);

    // Set name for back buffer for RenderDoc
    SetDebugName(backBuffer, "BackBuffer");

    SafeRelease(backBuffer);

    if (SUCCEEDED(hr))
    {
        // Set name for render target view for RenderDoc
        SetDebugName(m_renderTargetView, "RenderTargetView");
    }
    else
    {
        OutputDebugString((L"Failed to create render target view. HRESULT: " + std::to_wstring(hr) + L"\n").c_str());
    }

    return hr;
}

HRESULT Renderer::CreateDepthStencilBuffer()
{
    // Create depth stencil texture
    OutputDebugString(L"Creating depth stencil texture...\n");
    D3D11_TEXTURE2D_DESC depthStencilDesc = {};
    depthStencilDesc.Width = m_width;
    depthStencilDesc.Height = m_height;
    depthStencilDesc.MipLevels = 1;
    depthStencilDesc.ArraySize = 1;
    depthStencilDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthStencilDesc.SampleDesc.Count = 1;
    depthStencilDesc.SampleDesc.Quality = 0;
    depthStencilDesc.Usage = D3D11_USAGE_DEFAULT;
    depthStencilDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    depthStencilDesc.CPUAccessFlags = 0;
    depthStencilDesc.MiscFlags = 0;

    HRESULT hr = m_device->CreateTexture2D(&depthStencilDesc, nullptr, &m_depthStencilBuffer);
    if (FAILED(hr)) {
        OutputDebugString((L"Failed to create depth stencil texture. HRESULT: " + std::to_wstring(hr) + L"\n").c_str());
        return hr;
    }

    // Create depth stencil view
    OutputDebugString(L"Creating depth stencil view...\n");
    D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc = {};
    depthStencilViewDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthStencilViewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    depthStencilViewDesc.Texture2D.MipSlice = 0;

    hr = m_device->CreateDepthStencilView(m_depthStencilBuffer, &depthStencilViewDesc, &m_depthStencilView);

    // Set name for depth stencil buffer for RenderDoc
    SetDebugName(m_depthStencilBuffer, "DepthStencilBuffer");
    SetDebugName(m_depthStencilView, "DepthStencilView");

    if (FAILED(hr)) {
        OutputDebugString((L"Failed to create depth stencil view. HRESULT: " + std::to_wstring(hr) + L"\n").c_str());
    }

    return hr;
}

void Renderer::CreateViewport()
{
    OutputDebugString(L"Setting up viewport...\n");
    // Setup the viewport
    D3D11_VIEWPORT vp;
    ZeroMemory(&vp, sizeof(vp));
    vp.Width = (FLOAT)m_width;
    vp.Height = (FLOAT)m_height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    m_deviceContext->RSSetViewports(1, &vp);
}

HRESULT Renderer::CompileShader(const std::wstring& shaderFile, const std::string& entryPoint,
    const std::string& shaderModel, ID3DBlob** blobOut)
{
    DWORD shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    shaderFlags |= D3DCOMPILE_DEBUG;
    shaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ID3DBlob* errorBlob = nullptr;
    HRESULT hr = D3DCompileFromFile(
        shaderFile.c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint.c_str(),
        shaderModel.c_str(),
        shaderFlags,
        0,
        blobOut,
        &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            OutputDebugStringA((char*)errorBlob->GetBufferPointer());
            SafeRelease(errorBlob);
        }
        OutputDebugString((L"Failed to compile shader: " + shaderFile + L"\n").c_str());
        return hr;
    }

    if (errorBlob)
    {
        SafeRelease(errorBlob);
    }

    return S_OK;
}

HRESULT Renderer::LoadShaderBlob(const wchar_t* compiledShaderName, const wchar_t* sourceRelativePath,
    const char* entryPoint, const char* shaderModel, ID3DBlob** blobOut)
{
    if (blobOut == nullptr)
    {
        return E_POINTER;
    }

    *blobOut = nullptr;

    const fs::path executableDirectory = GetExecutableDirectory();

    std::error_code errorCode;
    const fs::path currentDirectory = fs::current_path(errorCode);

    std::vector<fs::path> compiledShaderCandidates;
    compiledShaderCandidates.emplace_back(executableDirectory / compiledShaderName);

    if (!currentDirectory.empty())
    {
        compiledShaderCandidates.emplace_back(currentDirectory / compiledShaderName);
    }

    for (const fs::path& shaderPath : compiledShaderCandidates)
    {
        if (!FileExists(shaderPath))
        {
            continue;
        }

        const HRESULT hr = D3DReadFileToBlob(shaderPath.c_str(), blobOut);
        if (SUCCEEDED(hr))
        {
            OutputDebugString((L"Loaded compiled shader: " + shaderPath.wstring() + L"\n").c_str());
            return S_OK;
        }
    }

    std::vector<fs::path> sourceShaderCandidates;
    if (!currentDirectory.empty())
    {
        sourceShaderCandidates.emplace_back(currentDirectory / sourceRelativePath);
        sourceShaderCandidates.emplace_back(currentDirectory / L"cube-renderer" / sourceRelativePath);
    }

    sourceShaderCandidates.emplace_back(executableDirectory / sourceRelativePath);

    const fs::path twoLevelsUp = executableDirectory.parent_path().parent_path();
    if (!twoLevelsUp.empty())
    {
        sourceShaderCandidates.emplace_back(twoLevelsUp / sourceRelativePath);
        sourceShaderCandidates.emplace_back(twoLevelsUp / L"cube-renderer" / sourceRelativePath);
    }

    for (const fs::path& shaderPath : sourceShaderCandidates)
    {
        if (!FileExists(shaderPath))
        {
            continue;
        }

        return CompileShader(shaderPath.wstring(), entryPoint, shaderModel, blobOut);
    }

    OutputDebugString((L"Could not locate shader source or bytecode for: " + std::wstring(sourceRelativePath) + L"\n").c_str());
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

HRESULT Renderer::CreateShaders()
{
    // Compile vertex shader
    OutputDebugString(L"Loading vertex shader...\n");
    ID3DBlob* vertexShaderBlob = nullptr;
    HRESULT hr = LoadShaderBlob(
        L"vertex_shader.cso",
        L"src\\shaders\\vertex_shader.hlsl",
        "VS",
        "vs_5_0",
        &vertexShaderBlob);
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to compile vertex shader\n");
        return hr;
    }

    // Create vertex shader
    OutputDebugString(L"Creating vertex shader...\n");
    hr = m_device->CreateVertexShader(
        vertexShaderBlob->GetBufferPointer(),
        vertexShaderBlob->GetBufferSize(),
        nullptr,
        &m_vertexShader);
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to create vertex shader\n");
        SafeRelease(vertexShaderBlob);
        return hr;
    }

    // Set name for vertex shader for RenderDoc
    SetDebugName(m_vertexShader, "VertexShader");

    // Define input layout
    D3D11_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    UINT numElements = ARRAYSIZE(layout);

    // Create input layout
    OutputDebugString(L"Creating input layout...\n");
    hr = m_device->CreateInputLayout(
        layout,
        numElements,
        vertexShaderBlob->GetBufferPointer(),
        vertexShaderBlob->GetBufferSize(),
        &m_inputLayout);
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to create input layout\n");
        SafeRelease(vertexShaderBlob);
        return hr;
    }

    // Set name for input layout for RenderDoc
    SetDebugName(m_inputLayout, "InputLayout");

    // Release vertex shader blob
    SafeRelease(vertexShaderBlob);

    // Compile pixel shader
    OutputDebugString(L"Loading pixel shader...\n");
    ID3DBlob* pixelShaderBlob = nullptr;
    hr = LoadShaderBlob(
        L"pixel_shader.cso",
        L"src\\shaders\\pixel_shader.hlsl",
        "PS",
        "ps_5_0",
        &pixelShaderBlob);
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to compile pixel shader\n");
        return hr;
    }

    // Create pixel shader
    OutputDebugString(L"Creating pixel shader...\n");
    hr = m_device->CreatePixelShader(
        pixelShaderBlob->GetBufferPointer(),
        pixelShaderBlob->GetBufferSize(),
        nullptr,
        &m_pixelShader);
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to create pixel shader\n");
        SafeRelease(pixelShaderBlob);
        return hr;
    }

    // Set name for pixel shader for RenderDoc
    SetDebugName(m_pixelShader, "PixelShader");

    // Release pixel shader blob
    SafeRelease(pixelShaderBlob);

    OutputDebugString(L"Shaders created successfully\n");
    return S_OK;
}

HRESULT Renderer::CreateConstantBuffer()
{
    OutputDebugString(L"Creating constant buffer...\n");
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.ByteWidth = sizeof(ConstantBuffer);
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = 0;

    HRESULT hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_constantBuffer);

    // Set name for constant buffer for RenderDoc
    SetDebugName(m_constantBuffer, "ConstantBuffer");

    if (FAILED(hr)) {
        OutputDebugString((L"Failed to create constant buffer. HRESULT: " + std::to_wstring(hr) + L"\n").c_str());
    }

    return hr;
}

HRESULT Renderer::CreateCubeGeometry()
{
    OutputDebugString(L"Creating cube geometry...\n");
    // Define vertices for a cube
    Vertex vertices[] =
    {
        // Front face
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
        { XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
        { XMFLOAT3(0.5f,  0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
        { XMFLOAT3(0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f) },

        // Back face
        { XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
        { XMFLOAT3(0.5f, -0.5f,  0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
        { XMFLOAT3(0.5f,  0.5f,  0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
        { XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f) },

        // Top face
        { XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(0.5f,  0.5f,  0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(0.5f,  0.5f, -0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },

        // Bottom face
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
        { XMFLOAT3(0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
        { XMFLOAT3(0.5f, -0.5f,  0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
        { XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f) },

        // Left face
        { XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },

        // Right face
        { XMFLOAT3(0.5f, -0.5f, -0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(0.5f,  0.5f, -0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(0.5f,  0.5f,  0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(0.5f, -0.5f,  0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
    };

    // Create vertex buffer
    D3D11_BUFFER_DESC vertexBufferDesc = {};
    vertexBufferDesc.Usage = D3D11_USAGE_DEFAULT;
    vertexBufferDesc.ByteWidth = sizeof(Vertex) * ARRAYSIZE(vertices);
    vertexBufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vertexBufferDesc.CPUAccessFlags = 0;
    vertexBufferDesc.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA vertexData = {};
    vertexData.pSysMem = vertices;
    vertexData.SysMemPitch = 0;
    vertexData.SysMemSlicePitch = 0;

    OutputDebugString(L"Creating vertex buffer...\n");
    HRESULT hr = m_device->CreateBuffer(&vertexBufferDesc, &vertexData, &m_vertexBuffer);
    if (FAILED(hr)) {
        OutputDebugString((L"Failed to create vertex buffer. HRESULT: " + std::to_wstring(hr) + L"\n").c_str());
        return hr;
    }

    // Set name for vertex buffer for RenderDoc
    SetDebugName(m_vertexBuffer, "VertexBuffer");

    // Define indices for a cube
    UINT indices[] =
    {
        // Front face
        0, 1, 2,
        0, 2, 3,

        // Back face
        4, 5, 6,
        4, 6, 7,

        // Top face
        8, 9, 10,
        8, 10, 11,

        // Bottom face
        12, 13, 14,
        12, 14, 15,

        // Left face
        16, 17, 18,
        16, 18, 19,

        // Right face
        20, 21, 22,
        20, 22, 23
    };

    m_indexCount = ARRAYSIZE(indices);

    // Create index buffer
    D3D11_BUFFER_DESC indexBufferDesc = {};
    indexBufferDesc.Usage = D3D11_USAGE_DEFAULT;
    indexBufferDesc.ByteWidth = sizeof(UINT) * m_indexCount;
    indexBufferDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    indexBufferDesc.CPUAccessFlags = 0;
    indexBufferDesc.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA indexData = {};
    indexData.pSysMem = indices;
    indexData.SysMemPitch = 0;
    indexData.SysMemSlicePitch = 0;

    OutputDebugString(L"Creating index buffer...\n");
    hr = m_device->CreateBuffer(&indexBufferDesc, &indexData, &m_indexBuffer);

    // Set name for index buffer for RenderDoc
    SetDebugName(m_indexBuffer, "IndexBuffer");

    if (FAILED(hr)) {
        OutputDebugString((L"Failed to create index buffer. HRESULT: " + std::to_wstring(hr) + L"\n").c_str());
    }

    return hr;
}

void Renderer::UpdateConstantBuffer()
{
    // Update rotation
    m_rotation += 0.0001f;

    // Create transformation matrices
    m_worldMatrix = XMMatrixRotationY(m_rotation);
    m_viewMatrix = XMMatrixLookAtLH(m_cameraPosition, m_cameraTarget, m_cameraUp);
    m_projectionMatrix = XMMatrixPerspectiveFovLH(XM_PIDIV4, (FLOAT)m_width / (FLOAT)m_height, 0.1f, 100.0f);

    // Update constant buffer
    ConstantBuffer cb;
    cb.world = XMMatrixTranspose(m_worldMatrix);
    cb.view = XMMatrixTranspose(m_viewMatrix);
    cb.projection = XMMatrixTranspose(m_projectionMatrix);

    m_deviceContext->UpdateSubresource(m_constantBuffer, 0, nullptr, &cb, 0, 0);
}

void Renderer::Render()
{
    if (m_deviceContext == nullptr ||
        m_swapChain == nullptr ||
        m_renderTargetView == nullptr ||
        m_depthStencilView == nullptr)
    {
        return;
    }

    BeginEvent(L"Frame");

    // Clear back buffer
    float clearColor[4] = { 0.0f, 0.125f, 0.3f, 1.0f };
    m_deviceContext->ClearRenderTargetView(m_renderTargetView, clearColor);

    // Clear depth buffer
    m_deviceContext->ClearDepthStencilView(m_depthStencilView, D3D11_CLEAR_DEPTH, 1.0f, 0);

    // Set render targets
    m_deviceContext->OMSetRenderTargets(1, &m_renderTargetView, m_depthStencilView);

    // Update constant buffer
    UpdateConstantBuffer();

    // Set shaders
    m_deviceContext->VSSetShader(m_vertexShader, nullptr, 0);
    m_deviceContext->PSSetShader(m_pixelShader, nullptr, 0);

    // Set input layout
    m_deviceContext->IASetInputLayout(m_inputLayout);

    // Set vertex buffer
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    m_deviceContext->IASetVertexBuffers(0, 1, &m_vertexBuffer, &stride, &offset);

    // Set index buffer
    m_deviceContext->IASetIndexBuffer(m_indexBuffer, DXGI_FORMAT_R32_UINT, 0);

    // Set primitive topology
    m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Set constant buffer
    m_deviceContext->VSSetConstantBuffers(0, 1, &m_constantBuffer);

    // Draw cube
    BeginEvent(L"DrawCube");
    m_deviceContext->DrawIndexed(m_indexCount, 0, 0);
    EndEvent();

    // Present back buffer
    const HRESULT hr = m_swapChain->Present(0, 0);
    if (FAILED(hr))
    {
        OutputDebugString((L"Present failed. HRESULT: " + std::to_wstring(hr) + L"\n").c_str());
    }

    EndEvent();
}

void Renderer::ResizeSwapChain(UINT width, UINT height)
{
    if (m_swapChain && width > 0 && height > 0)
    {
        m_deviceContext->OMSetRenderTargets(0, nullptr, nullptr);
        m_deviceContext->ClearState();
        m_deviceContext->Flush();

        // Release previous render target view
        SafeRelease(m_renderTargetView);

        // Release previous depth stencil view and buffer
        SafeRelease(m_depthStencilView);
        SafeRelease(m_depthStencilBuffer);

        // Resize swap chain
        const HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr))
        {
            OutputDebugString((L"ResizeBuffers failed. HRESULT: " + std::to_wstring(hr) + L"\n").c_str());
            return;
        }

        // Update width and height
        m_width = width;
        m_height = height;

        // Recreate render target view
        if (FAILED(CreateRenderTargetView()))
        {
            return;
        }

        // Recreate depth stencil buffer and view
        if (FAILED(CreateDepthStencilBuffer()))
        {
            return;
        }

        // Update viewport
        CreateViewport();

        m_deviceContext->RSSetState(m_rasterizerState);
        m_deviceContext->OMSetDepthStencilState(m_depthStencilState, 1);
    }
}

void Renderer::HandleMessages()
{
    MSG msg = {};
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

void Renderer::Cleanup()
{
    if (m_deviceContext != nullptr)
    {
        m_deviceContext->ClearState();
        m_deviceContext->Flush();
    }

    // Release all DirectX resources
    SafeRelease(m_vertexBuffer);
    SafeRelease(m_indexBuffer);
    SafeRelease(m_constantBuffer);
    SafeRelease(m_inputLayout);
    SafeRelease(m_vertexShader);
    SafeRelease(m_pixelShader);
    SafeRelease(m_annotation);
    SafeRelease(m_depthStencilState);
    SafeRelease(m_rasterizerState);
    SafeRelease(m_depthStencilView);
    SafeRelease(m_depthStencilBuffer);
    SafeRelease(m_renderTargetView);
    SafeRelease(m_swapChain);
    SafeRelease(m_deviceContext);
    SafeRelease(m_device);
}

LRESULT CALLBACK Renderer::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);

    Renderer* app = reinterpret_cast<Renderer*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (message)
    {
    case WM_SIZE:
        if (app)
        {
            app->ResizeSwapChain(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}

void Renderer::BeginEvent(const wchar_t* name) const
{
    if (m_annotation != nullptr && name != nullptr)
    {
        m_annotation->BeginEvent(name);
    }
}

void Renderer::EndEvent() const
{
    if (m_annotation != nullptr)
    {
        m_annotation->EndEvent();
    }
}
