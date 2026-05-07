#include "renderer.h"

#include <d3d11sdklayers.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <system_error>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
    namespace fs = std::filesystem;

    struct SceneVertex
    {
        XMFLOAT3 position;
        XMFLOAT3 normal;
    };

    constexpr float kPi = 3.1415926535f;
    constexpr UINT kSphereLatitudeSegments = 32;
    constexpr UINT kSphereLongitudeSegments = 64;
    constexpr UINT kEnvironmentCubemapSize = 64;
    constexpr UINT kEnvironmentCubemapMipLevels = 7;
    constexpr UINT kSphereGridWidth = 9;
    constexpr UINT kSphereGridHeight = 9;
    constexpr float kSphereGridSpacing = 1.12f;
    constexpr float kSphereScale = 0.38f;
    constexpr float kEnvironmentSphereScale = 80.0f;
    constexpr float kEnvironmentIntensity = 0.10f;
    constexpr float kCameraFieldOfViewDegrees = 32.0f;

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

    XMFLOAT4X4 StoreMatrix(const XMMATRIX& matrix)
    {
        XMFLOAT4X4 value = {};
        XMStoreFloat4x4(&value, matrix);
        return value;
    }

    XMFLOAT4X4 CreateWorldMatrix(const XMFLOAT3& scale, const XMFLOAT3& translation, float rotationY = 0.0f)
    {
        const XMMATRIX worldMatrix =
            XMMatrixScaling(scale.x, scale.y, scale.z) *
            XMMatrixRotationY(rotationY) *
            XMMatrixTranslation(translation.x, translation.y, translation.z);

        return StoreMatrix(worldMatrix);
    }

    template <typename T>
    HRESULT CreateConstantBuffer(ID3D11Device* device, ComPtr<ID3D11Buffer>& buffer, const char* debugName)
    {
        D3D11_BUFFER_DESC bufferDesc = {};
        bufferDesc.Usage = D3D11_USAGE_DEFAULT;
        bufferDesc.ByteWidth = (sizeof(T) + 15u) & ~15u;
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

        const HRESULT hr = device->CreateBuffer(&bufferDesc, nullptr, buffer.ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr))
        {
            SetDebugName(buffer.Get(), debugName);
        }

        return hr;
    }

    template <typename T>
    void UpdateConstantBuffer(ID3D11DeviceContext* deviceContext, const ComPtr<ID3D11Buffer>& buffer, const T& value)
    {
        deviceContext->UpdateSubresource(buffer.Get(), 0, nullptr, &value, 0, 0);
    }

    float Clamp01(float value)
    {
        return std::clamp(value, 0.0f, 1.0f);
    }

    XMFLOAT3 NormalizeFloat3(const XMFLOAT3& value)
    {
        const XMVECTOR vector = XMVector3Normalize(XMLoadFloat3(&value));
        XMFLOAT3 normalized = {};
        XMStoreFloat3(&normalized, vector);
        return normalized;
    }

    XMFLOAT3 EvaluateEnvironmentColor(const XMFLOAT3& direction)
    {
        const float skyBlend = Clamp01(direction.y * 0.5f + 0.5f);
        const XMVECTOR lowerColor = XMVectorSet(0.07f, 0.07f, 0.07f, 0.0f);
        const XMVECTOR upperColor = XMVectorSet(0.16f, 0.16f, 0.16f, 0.0f);

        XMFLOAT3 color = {};
        XMStoreFloat3(&color, XMVectorLerp(lowerColor, upperColor, skyBlend));
        return color;
    }

    XMFLOAT3 GetCubemapFaceDirection(UINT faceIndex, float u, float v)
    {
        XMFLOAT3 direction = {};

        switch (faceIndex)
        {
        case 0:
            direction = XMFLOAT3(1.0f, -v, -u);
            break;
        case 1:
            direction = XMFLOAT3(-1.0f, -v, u);
            break;
        case 2:
            direction = XMFLOAT3(u, 1.0f, v);
            break;
        case 3:
            direction = XMFLOAT3(u, -1.0f, -v);
            break;
        case 4:
            direction = XMFLOAT3(u, -v, 1.0f);
            break;
        default:
            direction = XMFLOAT3(-u, -v, -1.0f);
            break;
        }

        return NormalizeFloat3(direction);
    }
}

Renderer::Renderer() :
    m_hwnd(nullptr),
    m_hInstance(nullptr),
    m_width(1400),
    m_height(900),
    m_isMinimized(false),
    m_title(L"PBR Sphere"),
    m_isOrbiting(false),
    m_lastMousePosition{},
    m_cameraTarget(0.0f, 0.0f, 0.0f),
    m_cameraDistance(14.0f),
    m_cameraYaw(0.0f),
    m_cameraPitch(0.0f),
    m_cameraPosition(0.0f, 0.0f, 0.0f),
    m_displayMode(DisplayMode::Pbr),
    m_debugLayerEnabled(false)
{
    XMStoreFloat4x4(&m_viewMatrix, XMMatrixIdentity());
    XMStoreFloat4x4(&m_projectionMatrix, XMMatrixIdentity());
    UpdateCamera();
}

Renderer::~Renderer()
{
    Cleanup();
}

HRESULT Renderer::Initialize(HINSTANCE hInstance, int nCmdShow)
{
    m_hInstance = hInstance;

    HRESULT hr = RegisterWindowClass(hInstance);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateAppWindow(hInstance, nCmdShow);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = InitializeDirectX();
    if (FAILED(hr))
    {
        return hr;
    }

    CreateSceneObjects();
    InitializeLights();
    ResetCamera();
    UpdateWindowTitle();

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);

    return S_OK;
}

HRESULT Renderer::RegisterWindowClass(HINSTANCE hInstance)
{
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = hInstance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = L"PBRSphereWindowClass";

    if (!RegisterClassExW(&windowClass))
    {
        const DWORD error = GetLastError();
        if (error == ERROR_CLASS_ALREADY_EXISTS)
        {
            return S_OK;
        }

        return HRESULT_FROM_WIN32(error);
    }

    return S_OK;
}

HRESULT Renderer::CreateAppWindow(HINSTANCE hInstance, int nCmdShow)
{
    UNREFERENCED_PARAMETER(nCmdShow);

    RECT clientRect = { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    if (!AdjustWindowRect(&clientRect, WS_OVERLAPPEDWINDOW, FALSE))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    m_hwnd = CreateWindowExW(
        0,
        L"PBRSphereWindowClass",
        m_title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        clientRect.right - clientRect.left,
        clientRect.bottom - clientRect.top,
        nullptr,
        nullptr,
        hInstance,
        this);

    if (m_hwnd == nullptr)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    return S_OK;
}

HRESULT Renderer::InitializeDirectX()
{
    HRESULT hr = CreateDeviceAndContext();
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateSwapChain();
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateWindowSizeResources();
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreatePipelineStates();
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateSamplers();
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateConstantBuffers();
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateShaders();
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateGeometry();
    if (FAILED(hr))
    {
        return hr;
    }

    return CreateEnvironmentCubemap();
}

HRESULT Renderer::CreateDeviceAndContext()
{
    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    m_debugLayerEnabled = true;
#endif

    const D3D_DRIVER_TYPE driverTypes[] =
    {
        D3D_DRIVER_TYPE_HARDWARE,
        D3D_DRIVER_TYPE_WARP,
        D3D_DRIVER_TYPE_REFERENCE,
    };

    const D3D_FEATURE_LEVEL featureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_0,
    };

    HRESULT hr = E_FAIL;
    for (const D3D_DRIVER_TYPE driverType : driverTypes)
    {
        hr = D3D11CreateDevice(
            nullptr,
            driverType,
            nullptr,
            createDeviceFlags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            m_device.ReleaseAndGetAddressOf(),
            nullptr,
            m_deviceContext.ReleaseAndGetAddressOf());

#ifdef _DEBUG
        if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING && (createDeviceFlags & D3D11_CREATE_DEVICE_DEBUG) != 0)
        {
            m_debugLayerEnabled = false;
            m_device.Reset();
            m_deviceContext.Reset();

            hr = D3D11CreateDevice(
                nullptr,
                driverType,
                nullptr,
                createDeviceFlags & ~D3D11_CREATE_DEVICE_DEBUG,
                featureLevels,
                ARRAYSIZE(featureLevels),
                D3D11_SDK_VERSION,
                m_device.ReleaseAndGetAddressOf(),
                nullptr,
                m_deviceContext.ReleaseAndGetAddressOf());
        }
#endif

        if (SUCCEEDED(hr))
        {
            break;
        }
    }

    if (FAILED(hr))
    {
        return hr;
    }

#ifdef _DEBUG
    if (m_debugLayerEnabled)
    {
        ComPtr<ID3D11InfoQueue> infoQueue;
        if (SUCCEEDED(m_device.As(&infoQueue)))
        {
            infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
        }
    }
#endif

    m_deviceContext.As(&m_annotation);
    return S_OK;
}

HRESULT Renderer::CreateSwapChain()
{
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> dxgiAdapter;
    ComPtr<IDXGIFactory> dxgiFactory;

    HRESULT hr = m_device.As(&dxgiDevice);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = dxgiDevice->GetParent(IID_PPV_ARGS(dxgiAdapter.ReleaseAndGetAddressOf()));
    if (FAILED(hr))
    {
        return hr;
    }

    hr = dxgiAdapter->GetParent(IID_PPV_ARGS(dxgiFactory.ReleaseAndGetAddressOf()));
    if (FAILED(hr))
    {
        return hr;
    }

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
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    hr = dxgiFactory->CreateSwapChain(m_device.Get(), &swapChainDesc, m_swapChain.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    dxgiFactory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
    SetDebugName(m_swapChain.Get(), "PBRSphereSwapChain");

    return S_OK;
}

HRESULT Renderer::CreateWindowSizeResources()
{
    HRESULT hr = CreateRenderTargetView();
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateDepthStencilBuffer();
    if (FAILED(hr))
    {
        return hr;
    }

    SetViewport(m_width, m_height);
    return S_OK;
}

HRESULT Renderer::CreateRenderTargetView()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.ReleaseAndGetAddressOf()));
    if (FAILED(hr))
    {
        return hr;
    }

    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, m_renderTargetView.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    SetDebugName(backBuffer.Get(), "PBRSphereBackBuffer");
    SetDebugName(m_renderTargetView.Get(), "PBRSphereBackBufferRTV");
    return S_OK;
}

HRESULT Renderer::CreateDepthStencilBuffer()
{
    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = m_width;
    textureDesc.Height = m_height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    HRESULT hr = m_device->CreateTexture2D(&textureDesc, nullptr, m_depthStencilBuffer.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc = {};
    viewDesc.Format = textureDesc.Format;
    viewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.MipSlice = 0;

    hr = m_device->CreateDepthStencilView(
        m_depthStencilBuffer.Get(),
        &viewDesc,
        m_depthStencilView.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    SetDebugName(m_depthStencilBuffer.Get(), "PBRSphereDepthBuffer");
    SetDebugName(m_depthStencilView.Get(), "PBRSphereDepthBufferDSV");
    return S_OK;
}

void Renderer::ReleaseWindowSizeResources()
{
    m_renderTargetView.Reset();
    m_depthStencilView.Reset();
    m_depthStencilBuffer.Reset();
}

HRESULT Renderer::CreatePipelineStates()
{
    D3D11_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    // The generated sphere mesh uses the opposite winding from D3D's default front-face
    // convention. Disable culling so the visible hemisphere is shaded correctly.
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;

    HRESULT hr = m_device->CreateRasterizerState(&rasterizerDesc, m_rasterizerState.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_RASTERIZER_DESC skyRasterizerDesc = rasterizerDesc;
    skyRasterizerDesc.CullMode = D3D11_CULL_NONE;

    hr = m_device->CreateRasterizerState(&skyRasterizerDesc, m_skyRasterizerState.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;

    hr = m_device->CreateDepthStencilState(&depthStencilDesc, m_depthStencilState.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_DEPTH_STENCIL_DESC skyDepthStencilDesc = {};
    skyDepthStencilDesc.DepthEnable = FALSE;
    skyDepthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;

    hr = m_device->CreateDepthStencilState(&skyDepthStencilDesc, m_skyDepthStencilState.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    SetDebugName(m_rasterizerState.Get(), "PBRSphereRasterizerState");
    SetDebugName(m_skyRasterizerState.Get(), "PBRSphereSkyRasterizerState");
    SetDebugName(m_depthStencilState.Get(), "PBRSphereDepthStencilState");
    SetDebugName(m_skyDepthStencilState.Get(), "PBRSphereSkyDepthStencilState");
    return S_OK;
}

HRESULT Renderer::CreateSamplers()
{
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    const HRESULT hr = m_device->CreateSamplerState(&samplerDesc, m_linearClampSampler.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    SetDebugName(m_linearClampSampler.Get(), "PBRSphereLinearClampSampler");
    return S_OK;
}

HRESULT Renderer::CreateConstantBuffers()
{
    HRESULT hr = CreateConstantBuffer<SceneFrameConstants>(m_device.Get(), m_sceneFrameConstantBuffer, "PBRSphereSceneFrameCB");
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateConstantBuffer<SceneObjectConstants>(m_device.Get(), m_sceneObjectConstantBuffer, "PBRSphereSceneObjectCB");
    if (FAILED(hr))
    {
        return hr;
    }

    return CreateConstantBuffer<SkyFrameConstants>(m_device.Get(), m_skyFrameConstantBuffer, "PBRSphereSkyFrameCB");
}

HRESULT Renderer::CreateShaders()
{
    ComPtr<ID3DBlob> pbrSceneVertexShaderBlob;
    HRESULT hr = LoadShaderBlob(
        L"pbr_scene_vertex_shader.cso",
        L"src\\shaders\\pbr_scene_vertex_shader.hlsl",
        "VS",
        "vs_5_0",
        pbrSceneVertexShaderBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = m_device->CreateVertexShader(
        pbrSceneVertexShaderBlob->GetBufferPointer(),
        pbrSceneVertexShaderBlob->GetBufferSize(),
        nullptr,
        m_pbrSceneVertexShader.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_INPUT_ELEMENT_DESC inputLayoutDesc[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(SceneVertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(SceneVertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    hr = m_device->CreateInputLayout(
        inputLayoutDesc,
        ARRAYSIZE(inputLayoutDesc),
        pbrSceneVertexShaderBlob->GetBufferPointer(),
        pbrSceneVertexShaderBlob->GetBufferSize(),
        m_sceneInputLayout.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    auto createPixelShader = [this](
        const wchar_t* compiledShaderName,
        const wchar_t* sourceRelativePath,
        const char* debugName,
        ID3D11PixelShader** shaderOut) -> HRESULT
    {
        ComPtr<ID3DBlob> pixelShaderBlob;
        HRESULT localHr = LoadShaderBlob(
            compiledShaderName,
            sourceRelativePath,
            "PS",
            "ps_5_0",
            pixelShaderBlob.ReleaseAndGetAddressOf());
        if (FAILED(localHr))
        {
            return localHr;
        }

        localHr = m_device->CreatePixelShader(
            pixelShaderBlob->GetBufferPointer(),
            pixelShaderBlob->GetBufferSize(),
            nullptr,
            shaderOut);
        if (SUCCEEDED(localHr))
        {
            SetDebugName(*shaderOut, debugName);
        }

        return localHr;
    };

    hr = createPixelShader(
        L"pbr_scene_pixel_shader.cso",
        L"src\\shaders\\pbr_scene_pixel_shader.hlsl",
        "PBRSphereScenePixelShader",
        m_pbrScenePixelShader.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    ComPtr<ID3DBlob> skyVertexShaderBlob;
    hr = LoadShaderBlob(
        L"sky_vertex_shader.cso",
        L"src\\shaders\\sky_vertex_shader.hlsl",
        "VS",
        "vs_5_0",
        skyVertexShaderBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = m_device->CreateVertexShader(
        skyVertexShaderBlob->GetBufferPointer(),
        skyVertexShaderBlob->GetBufferSize(),
        nullptr,
        m_skyVertexShader.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = createPixelShader(
        L"sky_pixel_shader.cso",
        L"src\\shaders\\sky_pixel_shader.hlsl",
        "PBRSphereSkyPixelShader",
        m_skyPixelShader.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    SetDebugName(m_pbrSceneVertexShader.Get(), "PBRSphereSceneVertexShader");
    SetDebugName(m_skyVertexShader.Get(), "PBRSphereSkyVertexShader");
    SetDebugName(m_sceneInputLayout.Get(), "PBRSphereInputLayout");
    return S_OK;
}

HRESULT Renderer::CreateGeometry()
{
    std::vector<SceneVertex> vertices;
    std::vector<UINT> indices;

    vertices.reserve((kSphereLatitudeSegments + 1) * (kSphereLongitudeSegments + 1));
    indices.reserve(kSphereLatitudeSegments * kSphereLongitudeSegments * 6);

    for (UINT latitude = 0; latitude <= kSphereLatitudeSegments; ++latitude)
    {
        const float v = static_cast<float>(latitude) / static_cast<float>(kSphereLatitudeSegments);
        const float phi = v * kPi;
        const float y = std::cos(phi);
        const float ringRadius = std::sin(phi);

        for (UINT longitude = 0; longitude <= kSphereLongitudeSegments; ++longitude)
        {
            const float u = static_cast<float>(longitude) / static_cast<float>(kSphereLongitudeSegments);
            const float theta = u * kPi * 2.0f;

            const float x = std::cos(theta) * ringRadius;
            const float z = std::sin(theta) * ringRadius;
            vertices.push_back({ XMFLOAT3(x, y, z), XMFLOAT3(x, y, z) });
        }
    }

    const UINT stride = kSphereLongitudeSegments + 1;
    for (UINT latitude = 0; latitude < kSphereLatitudeSegments; ++latitude)
    {
        for (UINT longitude = 0; longitude < kSphereLongitudeSegments; ++longitude)
        {
            const UINT topLeft = latitude * stride + longitude;
            const UINT topRight = topLeft + 1;
            const UINT bottomLeft = (latitude + 1) * stride + longitude;
            const UINT bottomRight = bottomLeft + 1;

            indices.push_back(topLeft);
            indices.push_back(bottomLeft);
            indices.push_back(topRight);

            indices.push_back(topRight);
            indices.push_back(bottomLeft);
            indices.push_back(bottomRight);
        }
    }

    return CreateMeshGeometry(
        vertices.data(),
        sizeof(SceneVertex),
        static_cast<UINT>(vertices.size()),
        indices.data(),
        static_cast<UINT>(indices.size()),
        m_sphereGeometry,
        "PBRSphereMesh");
}

HRESULT Renderer::CreateEnvironmentCubemap()
{
    std::array<std::vector<std::uint8_t>, 6> faceData;

    const UINT facePitch = kEnvironmentCubemapSize * 4u;
    for (UINT faceIndex = 0; faceIndex < 6; ++faceIndex)
    {
        faceData[faceIndex].resize(kEnvironmentCubemapSize * kEnvironmentCubemapSize * 4u);

        for (UINT y = 0; y < kEnvironmentCubemapSize; ++y)
        {
            const float v = ((static_cast<float>(y) + 0.5f) / static_cast<float>(kEnvironmentCubemapSize)) * 2.0f - 1.0f;

            for (UINT x = 0; x < kEnvironmentCubemapSize; ++x)
            {
                const float u = ((static_cast<float>(x) + 0.5f) / static_cast<float>(kEnvironmentCubemapSize)) * 2.0f - 1.0f;
                const XMFLOAT3 direction = GetCubemapFaceDirection(faceIndex, u, v);
                const XMFLOAT3 color = EvaluateEnvironmentColor(direction);

                const size_t texelIndex = (static_cast<size_t>(y) * kEnvironmentCubemapSize + x) * 4u;
                faceData[faceIndex][texelIndex + 0] = static_cast<std::uint8_t>(Clamp01(color.x) * 255.0f);
                faceData[faceIndex][texelIndex + 1] = static_cast<std::uint8_t>(Clamp01(color.y) * 255.0f);
                faceData[faceIndex][texelIndex + 2] = static_cast<std::uint8_t>(Clamp01(color.z) * 255.0f);
                faceData[faceIndex][texelIndex + 3] = 255u;
            }
        }

    }

    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = kEnvironmentCubemapSize;
    textureDesc.Height = kEnvironmentCubemapSize;
    textureDesc.MipLevels = kEnvironmentCubemapMipLevels;
    textureDesc.ArraySize = 6;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    textureDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_GENERATE_MIPS;

    HRESULT hr = m_device->CreateTexture2D(&textureDesc, nullptr, m_environmentCubemap.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    for (UINT faceIndex = 0; faceIndex < 6; ++faceIndex)
    {
        const UINT subresourceIndex = D3D11CalcSubresource(0, faceIndex, kEnvironmentCubemapMipLevels);
        m_deviceContext->UpdateSubresource(
            m_environmentCubemap.Get(),
            subresourceIndex,
            nullptr,
            faceData[faceIndex].data(),
            facePitch,
            facePitch * kEnvironmentCubemapSize);
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc = {};
    shaderResourceViewDesc.Format = textureDesc.Format;
    shaderResourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    shaderResourceViewDesc.TextureCube.MipLevels = kEnvironmentCubemapMipLevels;
    shaderResourceViewDesc.TextureCube.MostDetailedMip = 0;

    hr = m_device->CreateShaderResourceView(
        m_environmentCubemap.Get(),
        &shaderResourceViewDesc,
        m_environmentCubemapShaderResourceView.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    m_deviceContext->GenerateMips(m_environmentCubemapShaderResourceView.Get());

    SetDebugName(m_environmentCubemap.Get(), "PBRSphereEnvironmentCubemap");
    SetDebugName(m_environmentCubemapShaderResourceView.Get(), "PBRSphereEnvironmentCubemapSRV");
    return S_OK;
}

void Renderer::CreateSceneObjects()
{
    m_sphereInstances.clear();
    m_sphereInstances.reserve(kSphereGridWidth * kSphereGridHeight);

    const float gridWidthOffset = (static_cast<float>(kSphereGridWidth) - 1.0f) * kSphereGridSpacing * 0.5f;
    const float gridHeightOffset = (static_cast<float>(kSphereGridHeight) - 1.0f) * kSphereGridSpacing * 0.5f;

    for (UINT row = 0; row < kSphereGridHeight; ++row)
    {
        for (UINT column = 0; column < kSphereGridWidth; ++column)
        {
            const float x = static_cast<float>(column) * kSphereGridSpacing - gridWidthOffset;
            const float y = static_cast<float>(row) * kSphereGridSpacing - gridHeightOffset;
            const float roughnessT = static_cast<float>(column) / static_cast<float>(kSphereGridWidth - 1u);
            const float metalnessT = static_cast<float>(row) / static_cast<float>(kSphereGridHeight - 1u);

            SphereInstance instance = {};
            instance.world = CreateWorldMatrix(
                XMFLOAT3(kSphereScale, kSphereScale, kSphereScale),
                XMFLOAT3(x, y, 0.0f));
            instance.albedo = XMFLOAT3(1.0f, 0.71f, 0.29f);
            instance.roughness = std::clamp(0.1f + roughnessT * 0.9f, 0.1f, 1.0f);
            instance.metalness = std::clamp(metalnessT, 0.0f, 1.0f);
            instance.emissiveStrength = 0.0f;

            m_sphereInstances.push_back(instance);
        }
    }
}

void Renderer::InitializeLights()
{
    // Light setup for the lecture-like previews:
    // - PBR / NDF / Geometry use an offset light to make the lobe easier to read.
    // - Fresnel uses a centered light halfway between the camera and the grid.
    m_pointLights[0] = { XMFLOAT3(-2.2f, 5.0f, -4.8f), 18.0f, XMFLOAT3(1.0f, 1.0f, 1.0f), 1500.0f };
    m_pointLights[1] = { XMFLOAT3(0.0f, 0.0f, -7.0f), 18.0f, XMFLOAT3(1.0f, 1.0f, 1.0f), 900.0f };
    m_pointLights[2] = { XMFLOAT3(0.0f, 0.0f, 0.0f), 1.0f, XMFLOAT3(0.0f, 0.0f, 0.0f), 0.0f };
}

void Renderer::UpdateWindowTitle()
{
    if (m_hwnd == nullptr)
    {
        return;
    }

    const wchar_t* modeName = L"PBR";
    switch (m_displayMode)
    {
    case DisplayMode::NormalDistribution:
        modeName = L"NDF";
        break;
    case DisplayMode::Geometry:
        modeName = L"Geometry";
        break;
    case DisplayMode::Fresnel:
        modeName = L"Fresnel";
        break;
    case DisplayMode::Pbr:
    default:
        modeName = L"PBR";
        break;
    }

    const std::wstring title =
        m_title +
        L" | 1 PBR | 2 NDF | 3 Geometry | 4 Fresnel | Light sphere marks point source | Roughness: left->right | Metalness: bottom->top | LMB orbit | Wheel zoom | R reset | Mode " +
        std::wstring(modeName);

    SetWindowTextW(m_hwnd, title.c_str());
}

void Renderer::ResetCamera()
{
    m_cameraTarget = XMFLOAT3(0.0f, 0.0f, 0.0f);
    m_cameraDistance = 14.0f;
    m_cameraYaw = 0.0f;
    m_cameraPitch = 0.0f;
    UpdateCamera();
}

void Renderer::UpdateCamera()
{
    const XMVECTOR target = XMLoadFloat3(&m_cameraTarget);
    const float cosPitch = std::cos(m_cameraPitch);

    const XMVECTOR offset = XMVectorSet(
        std::sin(m_cameraYaw) * cosPitch,
        std::sin(m_cameraPitch),
        std::cos(m_cameraYaw) * cosPitch,
        0.0f);

    const XMVECTOR cameraPosition = XMVectorSubtract(target, XMVectorScale(offset, m_cameraDistance));
    XMStoreFloat3(&m_cameraPosition, cameraPosition);

    const XMMATRIX viewMatrix = XMMatrixLookAtLH(cameraPosition, target, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const float aspectRatio = (m_height > 0) ? (static_cast<float>(m_width) / static_cast<float>(m_height)) : 1.0f;
    const XMMATRIX projectionMatrix =
        XMMatrixPerspectiveFovLH(XMConvertToRadians(kCameraFieldOfViewDegrees), aspectRatio, 0.1f, 300.0f);

    XMStoreFloat4x4(&m_viewMatrix, viewMatrix);
    XMStoreFloat4x4(&m_projectionMatrix, projectionMatrix);
}

void Renderer::RenderEnvironment()
{
    BeginEvent(L"EnvironmentPass");

    SkyFrameConstants skyFrameConstants = {};
    skyFrameConstants.viewProjection = StoreMatrix(XMLoadFloat4x4(&m_viewMatrix) * XMLoadFloat4x4(&m_projectionMatrix));
    skyFrameConstants.cameraPosition = XMFLOAT4(m_cameraPosition.x, m_cameraPosition.y, m_cameraPosition.z, 1.0f);
    UpdateConstantBuffer(m_deviceContext.Get(), m_skyFrameConstantBuffer, skyFrameConstants);

    const UINT stride = sizeof(SceneVertex);
    const UINT offset = 0;
    ID3D11Buffer* vertexBuffers[] = { m_sphereGeometry.vertexBuffer.Get() };
    m_deviceContext->IASetInputLayout(m_sceneInputLayout.Get());
    m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_deviceContext->IASetVertexBuffers(0, 1, vertexBuffers, &stride, &offset);
    m_deviceContext->IASetIndexBuffer(m_sphereGeometry.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);

    m_deviceContext->RSSetState(m_skyRasterizerState.Get());
    m_deviceContext->OMSetDepthStencilState(m_skyDepthStencilState.Get(), 0);
    m_deviceContext->VSSetShader(m_skyVertexShader.Get(), nullptr, 0);
    m_deviceContext->PSSetShader(m_skyPixelShader.Get(), nullptr, 0);

    ID3D11Buffer* skyBuffers[] = { m_skyFrameConstantBuffer.Get() };
    m_deviceContext->VSSetConstantBuffers(0, 1, skyBuffers);
    m_deviceContext->PSSetConstantBuffers(0, 1, skyBuffers);

    ID3D11SamplerState* samplers[] = { m_linearClampSampler.Get() };
    m_deviceContext->PSSetSamplers(0, 1, samplers);

    ID3D11ShaderResourceView* shaderResources[] = { m_environmentCubemapShaderResourceView.Get() };
    m_deviceContext->PSSetShaderResources(0, 1, shaderResources);
    m_deviceContext->DrawIndexed(m_sphereGeometry.indexCount, 0, 0);

    ID3D11ShaderResourceView* nullSrvs[] = { nullptr };
    m_deviceContext->PSSetShaderResources(0, 1, nullSrvs);

    EndEvent();
}

void Renderer::RenderSpheres()
{
    BeginEvent(L"PBRSpherePass");

    std::array<PointLight, 3> activeLights = {};
    for (PointLight& light : activeLights)
    {
        light = { XMFLOAT3(0.0f, 0.0f, 0.0f), 1.0f, XMFLOAT3(0.0f, 0.0f, 0.0f), 0.0f };
    }

    if (m_displayMode == DisplayMode::Fresnel)
    {
        activeLights[0] = m_pointLights[1];
    }
    else
    {
        activeLights[0] = m_pointLights[0];
    }

    SceneFrameConstants sceneFrameConstants = {};
    sceneFrameConstants.viewProjection = StoreMatrix(XMLoadFloat4x4(&m_viewMatrix) * XMLoadFloat4x4(&m_projectionMatrix));
    sceneFrameConstants.cameraPosition = XMFLOAT4(m_cameraPosition.x, m_cameraPosition.y, m_cameraPosition.z, 1.0f);

    for (size_t lightIndex = 0; lightIndex < activeLights.size(); ++lightIndex)
    {
        const PointLight& light = activeLights[lightIndex];
        sceneFrameConstants.pointLights[lightIndex].positionRadius =
            XMFLOAT4(light.position.x, light.position.y, light.position.z, light.radius);
        sceneFrameConstants.pointLights[lightIndex].colorIntensity =
            XMFLOAT4(light.color.x, light.color.y, light.color.z, light.intensity);
    }

    sceneFrameConstants.globalParameters =
        XMFLOAT4(
            static_cast<float>(static_cast<int>(m_displayMode)),
            kEnvironmentIntensity,
            kSphereGridSpacing,
            kSphereScale);
    UpdateConstantBuffer(m_deviceContext.Get(), m_sceneFrameConstantBuffer, sceneFrameConstants);

    const UINT stride = sizeof(SceneVertex);
    const UINT offset = 0;
    ID3D11Buffer* vertexBuffers[] = { m_sphereGeometry.vertexBuffer.Get() };
    m_deviceContext->IASetInputLayout(m_sceneInputLayout.Get());
    m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_deviceContext->IASetVertexBuffers(0, 1, vertexBuffers, &stride, &offset);
    m_deviceContext->IASetIndexBuffer(m_sphereGeometry.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);

    m_deviceContext->RSSetState(m_rasterizerState.Get());
    m_deviceContext->OMSetDepthStencilState(m_depthStencilState.Get(), 0);
    m_deviceContext->VSSetShader(m_pbrSceneVertexShader.Get(), nullptr, 0);
    m_deviceContext->PSSetShader(m_pbrScenePixelShader.Get(), nullptr, 0);

    ID3D11Buffer* sceneFrameBuffers[] = { m_sceneFrameConstantBuffer.Get() };
    m_deviceContext->VSSetConstantBuffers(0, 1, sceneFrameBuffers);
    m_deviceContext->PSSetConstantBuffers(0, 1, sceneFrameBuffers);

    ID3D11SamplerState* samplers[] = { m_linearClampSampler.Get() };
    m_deviceContext->PSSetSamplers(0, 1, samplers);

    ID3D11ShaderResourceView* shaderResources[] = { m_environmentCubemapShaderResourceView.Get() };
    m_deviceContext->PSSetShaderResources(0, 1, shaderResources);

    for (const SphereInstance& instance : m_sphereInstances)
    {
        DrawSphereInstance(instance);
    }

    for (size_t lightIndex = 0; lightIndex < activeLights.size(); ++lightIndex)
    {
        if (m_displayMode == DisplayMode::NormalDistribution || m_displayMode == DisplayMode::Geometry)
        {
            continue;
        }

        const PointLight& light = activeLights[lightIndex];
        if (light.intensity <= 0.0f)
        {
            continue;
        }

        SphereInstance marker = {};
        marker.world = CreateWorldMatrix(
            XMFLOAT3(0.08f, 0.08f, 0.08f),
            light.position);
        marker.albedo = light.color;
        marker.roughness = 0.05f;
        marker.metalness = 0.0f;
        marker.emissiveStrength = 5.0f;
        DrawSphereInstance(marker);
    }

    ID3D11ShaderResourceView* nullSrvs[] = { nullptr };
    m_deviceContext->PSSetShaderResources(0, 1, nullSrvs);

    EndEvent();
}

void Renderer::DrawSphereInstance(const SphereInstance& object)
{
    const XMMATRIX worldMatrix = XMLoadFloat4x4(&object.world);

    // This project uses row-vector matrix math, so normals are transformed by the inverse matrix.
    const XMMATRIX normalMatrix = XMMatrixInverse(nullptr, worldMatrix);

    SceneObjectConstants objectConstants = {};
    objectConstants.world = StoreMatrix(worldMatrix);
    objectConstants.normalMatrix = StoreMatrix(normalMatrix);
    objectConstants.albedo = XMFLOAT4(object.albedo.x, object.albedo.y, object.albedo.z, 1.0f);
    objectConstants.materialParameters = XMFLOAT4(object.roughness, object.metalness, object.emissiveStrength, 0.0f);
    UpdateConstantBuffer(m_deviceContext.Get(), m_sceneObjectConstantBuffer, objectConstants);

    ID3D11Buffer* objectBuffers[] = { m_sceneObjectConstantBuffer.Get() };
    m_deviceContext->VSSetConstantBuffers(1, 1, objectBuffers);
    m_deviceContext->PSSetConstantBuffers(1, 1, objectBuffers);
    m_deviceContext->DrawIndexed(m_sphereGeometry.indexCount, 0, 0);
}

HRESULT Renderer::CompileShader(const std::wstring& shaderFile, const std::string& entryPoint,
    const std::string& shaderModel, ID3DBlob** blobOut)
{
    if (blobOut == nullptr)
    {
        return E_POINTER;
    }

    UINT shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    shaderFlags |= D3DCOMPILE_DEBUG;
    shaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errorBlob;
    const HRESULT hr = D3DCompileFromFile(
        shaderFile.c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint.c_str(),
        shaderModel.c_str(),
        shaderFlags,
        0,
        blobOut,
        errorBlob.GetAddressOf());

    if (FAILED(hr) && errorBlob != nullptr)
    {
        OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
    }

    return hr;
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
            return S_OK;
        }
    }

    std::vector<fs::path> sourceShaderCandidates;
    if (!currentDirectory.empty())
    {
        sourceShaderCandidates.emplace_back(currentDirectory / sourceRelativePath);
        sourceShaderCandidates.emplace_back(currentDirectory / L"pbr-sphere" / sourceRelativePath);
    }

    sourceShaderCandidates.emplace_back(executableDirectory / sourceRelativePath);

    const fs::path twoLevelsUp = executableDirectory.parent_path().parent_path();
    if (!twoLevelsUp.empty())
    {
        sourceShaderCandidates.emplace_back(twoLevelsUp / sourceRelativePath);
        sourceShaderCandidates.emplace_back(twoLevelsUp / L"pbr-sphere" / sourceRelativePath);
    }

    for (const fs::path& shaderPath : sourceShaderCandidates)
    {
        if (!FileExists(shaderPath))
        {
            continue;
        }

        return CompileShader(shaderPath.wstring(), entryPoint, shaderModel, blobOut);
    }

    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

HRESULT Renderer::CreateMeshGeometry(const void* vertexData, UINT vertexStride, UINT vertexCount,
    const UINT* indexData, UINT indexCount, MeshGeometry& geometry, const char* debugName)
{
    geometry = {};

    D3D11_BUFFER_DESC vertexBufferDesc = {};
    vertexBufferDesc.Usage = D3D11_USAGE_DEFAULT;
    vertexBufferDesc.ByteWidth = vertexStride * vertexCount;
    vertexBufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vertexSubresourceData = {};
    vertexSubresourceData.pSysMem = vertexData;

    HRESULT hr = m_device->CreateBuffer(
        &vertexBufferDesc,
        &vertexSubresourceData,
        geometry.vertexBuffer.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_BUFFER_DESC indexBufferDesc = {};
    indexBufferDesc.Usage = D3D11_USAGE_DEFAULT;
    indexBufferDesc.ByteWidth = sizeof(UINT) * indexCount;
    indexBufferDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA indexSubresourceData = {};
    indexSubresourceData.pSysMem = indexData;

    hr = m_device->CreateBuffer(
        &indexBufferDesc,
        &indexSubresourceData,
        geometry.indexBuffer.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    geometry.indexCount = indexCount;
    SetDebugName(geometry.vertexBuffer.Get(), std::string(debugName) + ".VertexBuffer");
    SetDebugName(geometry.indexBuffer.Get(), std::string(debugName) + ".IndexBuffer");
    return S_OK;
}

void Renderer::SetViewport(UINT width, UINT height) const
{
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<FLOAT>(width);
    viewport.Height = static_cast<FLOAT>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_deviceContext->RSSetViewports(1, &viewport);
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

void Renderer::Render()
{
    if (m_isMinimized || m_width == 0 || m_height == 0)
    {
        Sleep(16);
        return;
    }

    BeginEvent(L"Frame");

    ID3D11RenderTargetView* renderTargets[] = { m_renderTargetView.Get() };
    m_deviceContext->OMSetRenderTargets(1, renderTargets, m_depthStencilView.Get());
    SetViewport(m_width, m_height);

    const float clearColor[4] = { 0.02f, 0.02f, 0.025f, 1.0f };
    m_deviceContext->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);
    m_deviceContext->ClearDepthStencilView(m_depthStencilView.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    RenderEnvironment();
    RenderSpheres();

    const HRESULT hr = m_swapChain->Present(1, 0);
    if (FAILED(hr))
    {
        OutputDebugStringW((L"Present failed. HRESULT: " + std::to_wstring(hr) + L"\n").c_str());
    }

    EndEvent();
}

void Renderer::ResizeSwapChain(UINT width, UINT height)
{
    if (m_swapChain == nullptr || width == 0 || height == 0)
    {
        return;
    }

    m_deviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    m_deviceContext->ClearState();
    m_deviceContext->Flush();

    ReleaseWindowSizeResources();

    const HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr))
    {
        OutputDebugStringW((L"ResizeBuffers failed. HRESULT: " + std::to_wstring(hr) + L"\n").c_str());
        return;
    }

    m_width = width;
    m_height = height;
    CreateWindowSizeResources();
    UpdateCamera();
}

void Renderer::Cleanup()
{
    if (m_deviceContext != nullptr)
    {
        m_deviceContext->ClearState();
        m_deviceContext->Flush();
    }

    ReleaseWindowSizeResources();

    m_sphereGeometry = {};
    m_sceneFrameConstantBuffer.Reset();
    m_sceneObjectConstantBuffer.Reset();
    m_skyFrameConstantBuffer.Reset();
    m_sceneInputLayout.Reset();
    m_pbrSceneVertexShader.Reset();
    m_pbrScenePixelShader.Reset();
    m_skyVertexShader.Reset();
    m_skyPixelShader.Reset();
    m_linearClampSampler.Reset();
    m_environmentCubemapShaderResourceView.Reset();
    m_environmentCubemap.Reset();
    m_skyDepthStencilState.Reset();
    m_depthStencilState.Reset();
    m_skyRasterizerState.Reset();
    m_rasterizerState.Reset();
    m_annotation.Reset();
    m_swapChain.Reset();
    m_deviceContext.Reset();
    m_device.Reset();
}

LRESULT Renderer::HandleWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
        {
            m_isMinimized = true;
            return 0;
        }

        m_isMinimized = false;
        ResizeSwapChain(static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)));
        return 0;

    case WM_LBUTTONDOWN:
        m_isOrbiting = true;
        m_lastMousePosition = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        SetCapture(hwnd);
        return 0;

    case WM_LBUTTONUP:
        m_isOrbiting = false;
        if (GetCapture() == hwnd)
        {
            ReleaseCapture();
        }
        return 0;

    case WM_MOUSEMOVE:
        if (m_isOrbiting)
        {
            const POINT currentMousePosition = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const LONG deltaX = currentMousePosition.x - m_lastMousePosition.x;
            const LONG deltaY = currentMousePosition.y - m_lastMousePosition.y;

            constexpr float rotationSpeed = 0.01f;
            m_cameraYaw += static_cast<float>(deltaX) * rotationSpeed;
            m_cameraPitch = std::clamp(m_cameraPitch - static_cast<float>(deltaY) * rotationSpeed, -1.35f, 1.35f);
            m_lastMousePosition = currentMousePosition;

            UpdateCamera();
        }
        return 0;

    case WM_MOUSEWHEEL:
    {
        const float wheelDelta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA);
        m_cameraDistance = std::clamp(m_cameraDistance - wheelDelta * 1.25f, 5.0f, 40.0f);
        UpdateCamera();
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == '1')
        {
            m_displayMode = DisplayMode::Pbr;
            UpdateWindowTitle();
        }
        else if (wParam == '2')
        {
            m_displayMode = DisplayMode::NormalDistribution;
            UpdateWindowTitle();
        }
        else if (wParam == '3')
        {
            m_displayMode = DisplayMode::Geometry;
            UpdateWindowTitle();
        }
        else if (wParam == '4')
        {
            m_displayMode = DisplayMode::Fresnel;
            UpdateWindowTitle();
        }
        else if (wParam == 'R')
        {
            ResetCamera();
        }
        return 0;

    case WM_KILLFOCUS:
        m_isOrbiting = false;
        if (GetCapture() == hwnd)
        {
            ReleaseCapture();
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK Renderer::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        const CREATESTRUCTW* createStruct = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        Renderer* renderer = static_cast<Renderer*>(createStruct->lpCreateParams);
        renderer->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(renderer));
        return TRUE;
    }

    Renderer* renderer = reinterpret_cast<Renderer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (renderer != nullptr)
    {
        return renderer->HandleWindowMessage(hwnd, message, wParam, lParam);
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}
