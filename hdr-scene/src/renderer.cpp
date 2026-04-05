#include "renderer.h"

#include <d3d11sdklayers.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
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

    constexpr std::array<float, 3> kLightIntensityLevels = { 1.0f, 10.0f, 100.0f };
    constexpr float kAmbientStrength = 0.08f;
    constexpr float kMiddleGray = 0.18f;
    constexpr float kWhitePoint = 11.2f;
    constexpr float kAdaptationRate = 1.75f;
    constexpr float kMinLuminance = 0.001f;

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
}

Renderer::Renderer() :
    m_hwnd(nullptr),
    m_hInstance(nullptr),
    m_width(1280),
    m_height(720),
    m_isMinimized(false),
    m_title(L"HDR Scene"),
    m_previousFrameTime(std::chrono::steady_clock::now()),
    m_keyStates{},
    m_isOrbiting(false),
    m_lastMousePosition{},
    m_cameraTarget(0.0f, 0.6f, 0.0f),
    m_cameraDistance(9.0f),
    m_cameraYaw(0.0f),
    m_cameraPitch(-0.6f),
    m_cameraPosition(0.0f, 0.0f, 0.0f),
    m_lightIntensityIndex(1),
    m_currentAdaptedLuminanceIndex(0),
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
    UpdateCamera();
    UpdateWindowTitle();

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);
    m_previousFrameTime = std::chrono::steady_clock::now();

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
    windowClass.lpszClassName = L"HDRSceneWindowClass";

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
        L"HDRSceneWindowClass",
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

    return CreateGeometry();
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
    SetDebugName(m_swapChain.Get(), "HDRSceneSwapChain");

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

    hr = CreateHdrSceneBuffer();
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateLuminanceResources();
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

    SetDebugName(backBuffer.Get(), "HDRSceneBackBuffer");
    SetDebugName(m_renderTargetView.Get(), "HDRSceneBackBufferRTV");
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

    SetDebugName(m_depthStencilBuffer.Get(), "HDRSceneDepthBuffer");
    SetDebugName(m_depthStencilView.Get(), "HDRSceneDepthBufferDSV");
    return S_OK;
}

HRESULT Renderer::CreateHdrSceneBuffer()
{
    return CreateRenderTexture(
        m_width,
        m_height,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        m_hdrSceneTexture,
        "HDRSceneColor");
}

HRESULT Renderer::CreateLuminanceResources()
{
    m_luminanceChain.clear();

    UINT luminanceWidth = std::max(1u, (m_width + 1u) / 2u);
    UINT luminanceHeight = std::max(1u, (m_height + 1u) / 2u);

    for (UINT index = 0;; ++index)
    {
        RenderTexture texture;
        const std::string debugName = "LuminanceChain" + std::to_string(index);

        HRESULT hr = CreateRenderTexture(
            luminanceWidth,
            luminanceHeight,
            DXGI_FORMAT_R16_FLOAT,
            texture,
            debugName.c_str());
        if (FAILED(hr))
        {
            return hr;
        }

        m_luminanceChain.push_back(std::move(texture));

        if (luminanceWidth == 1 && luminanceHeight == 1)
        {
            break;
        }

        luminanceWidth = std::max(1u, (luminanceWidth + 1u) / 2u);
        luminanceHeight = std::max(1u, (luminanceHeight + 1u) / 2u);
    }

    for (UINT index = 0; index < m_adaptedLuminanceTextures.size(); ++index)
    {
        const std::string debugName = "AdaptedLuminance" + std::to_string(index);
        HRESULT hr = CreateRenderTexture(
            1,
            1,
            DXGI_FORMAT_R16_FLOAT,
            m_adaptedLuminanceTextures[index],
            debugName.c_str());
        if (FAILED(hr))
        {
            return hr;
        }
    }

    const float initialLuminance[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    for (RenderTexture& texture : m_adaptedLuminanceTextures)
    {
        m_deviceContext->ClearRenderTargetView(texture.renderTargetView.Get(), initialLuminance);
    }

    m_currentAdaptedLuminanceIndex = 0;
    return S_OK;
}

void Renderer::ReleaseWindowSizeResources()
{
    m_renderTargetView.Reset();
    m_depthStencilView.Reset();
    m_depthStencilBuffer.Reset();
    m_hdrSceneTexture = {};
    m_luminanceChain.clear();
    m_adaptedLuminanceTextures = {};
}

HRESULT Renderer::CreatePipelineStates()
{
    D3D11_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_BACK;
    rasterizerDesc.DepthClipEnable = TRUE;

    HRESULT hr = m_device->CreateRasterizerState(&rasterizerDesc, m_rasterizerState.ReleaseAndGetAddressOf());
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

    SetDebugName(m_rasterizerState.Get(), "HDRSceneRasterizerState");
    SetDebugName(m_depthStencilState.Get(), "HDRSceneDepthStencilState");
    return S_OK;
}

HRESULT Renderer::CreateSamplers()
{
    D3D11_SAMPLER_DESC pointSamplerDesc = {};
    pointSamplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    pointSamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    pointSamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    pointSamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    pointSamplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    HRESULT hr = m_device->CreateSamplerState(&pointSamplerDesc, m_pointClampSampler.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_SAMPLER_DESC linearSamplerDesc = pointSamplerDesc;
    linearSamplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;

    hr = m_device->CreateSamplerState(&linearSamplerDesc, m_linearClampSampler.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    SetDebugName(m_pointClampSampler.Get(), "HDRScenePointClampSampler");
    SetDebugName(m_linearClampSampler.Get(), "HDRSceneLinearClampSampler");
    return S_OK;
}

HRESULT Renderer::CreateConstantBuffers()
{
    HRESULT hr = CreateConstantBuffer<SceneFrameConstants>(m_device.Get(), m_sceneFrameConstantBuffer, "SceneFrameCB");
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateConstantBuffer<SceneObjectConstants>(m_device.Get(), m_sceneObjectConstantBuffer, "SceneObjectCB");
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateConstantBuffer<DownsampleConstants>(m_device.Get(), m_downsampleConstantBuffer, "DownsampleCB");
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateConstantBuffer<AdaptationConstants>(m_device.Get(), m_adaptationConstantBuffer, "AdaptationCB");
    if (FAILED(hr))
    {
        return hr;
    }

    return CreateConstantBuffer<ToneMappingConstants>(m_device.Get(), m_toneMappingConstantBuffer, "ToneMappingCB");
}

HRESULT Renderer::CreateShaders()
{
    ComPtr<ID3DBlob> sceneVertexShaderBlob;
    HRESULT hr = LoadShaderBlob(
        L"scene_vertex_shader.cso",
        L"src\\shaders\\scene_vertex_shader.hlsl",
        "VS",
        "vs_5_0",
        sceneVertexShaderBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = m_device->CreateVertexShader(
        sceneVertexShaderBlob->GetBufferPointer(),
        sceneVertexShaderBlob->GetBufferSize(),
        nullptr,
        m_sceneVertexShader.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    const D3D11_INPUT_ELEMENT_DESC inputLayoutDesc[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    hr = m_device->CreateInputLayout(
        inputLayoutDesc,
        ARRAYSIZE(inputLayoutDesc),
        sceneVertexShaderBlob->GetBufferPointer(),
        sceneVertexShaderBlob->GetBufferSize(),
        m_sceneInputLayout.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    SetDebugName(m_sceneVertexShader.Get(), "SceneVertexShader");
    SetDebugName(m_sceneInputLayout.Get(), "SceneInputLayout");

    ComPtr<ID3DBlob> scenePixelShaderBlob;
    hr = LoadShaderBlob(
        L"scene_pixel_shader.cso",
        L"src\\shaders\\scene_pixel_shader.hlsl",
        "PS",
        "ps_5_0",
        scenePixelShaderBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = m_device->CreatePixelShader(
        scenePixelShaderBlob->GetBufferPointer(),
        scenePixelShaderBlob->GetBufferSize(),
        nullptr,
        m_scenePixelShader.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    SetDebugName(m_scenePixelShader.Get(), "ScenePixelShader");

    auto createFullscreenVertexShader = [this](const wchar_t* compiledName, const wchar_t* sourceName,
        ComPtr<ID3D11VertexShader>& shader, const char* debugName) -> HRESULT
    {
        ComPtr<ID3DBlob> shaderBlob;
        HRESULT result = LoadShaderBlob(compiledName, sourceName, "VS", "vs_5_0", shaderBlob.ReleaseAndGetAddressOf());
        if (FAILED(result))
        {
            return result;
        }

        result = m_device->CreateVertexShader(
            shaderBlob->GetBufferPointer(),
            shaderBlob->GetBufferSize(),
            nullptr,
            shader.ReleaseAndGetAddressOf());
        if (SUCCEEDED(result))
        {
            SetDebugName(shader.Get(), debugName);
        }

        return result;
    };

    auto createPixelShader = [this](const wchar_t* compiledName, const wchar_t* sourceName,
        ComPtr<ID3D11PixelShader>& shader, const char* debugName) -> HRESULT
    {
        ComPtr<ID3DBlob> shaderBlob;
        HRESULT result = LoadShaderBlob(compiledName, sourceName, "PS", "ps_5_0", shaderBlob.ReleaseAndGetAddressOf());
        if (FAILED(result))
        {
            return result;
        }

        result = m_device->CreatePixelShader(
            shaderBlob->GetBufferPointer(),
            shaderBlob->GetBufferSize(),
            nullptr,
            shader.ReleaseAndGetAddressOf());
        if (SUCCEEDED(result))
        {
            SetDebugName(shader.Get(), debugName);
        }

        return result;
    };

    hr = createFullscreenVertexShader(
        L"fullscreen_vertex_shader.cso",
        L"src\\shaders\\fullscreen_vertex_shader.hlsl",
        m_fullscreenVertexShader,
        "FullscreenVertexShader");
    if (FAILED(hr))
    {
        return hr;
    }

    hr = createPixelShader(
        L"luminance_initial_pixel_shader.cso",
        L"src\\shaders\\luminance_initial_pixel_shader.hlsl",
        m_luminanceInitialPixelShader,
        "LuminanceInitialPixelShader");
    if (FAILED(hr))
    {
        return hr;
    }

    hr = createPixelShader(
        L"luminance_reduce_pixel_shader.cso",
        L"src\\shaders\\luminance_reduce_pixel_shader.hlsl",
        m_luminanceReducePixelShader,
        "LuminanceReducePixelShader");
    if (FAILED(hr))
    {
        return hr;
    }

    hr = createPixelShader(
        L"luminance_adapt_pixel_shader.cso",
        L"src\\shaders\\luminance_adapt_pixel_shader.hlsl",
        m_luminanceAdaptPixelShader,
        "LuminanceAdaptPixelShader");
    if (FAILED(hr))
    {
        return hr;
    }

    return createPixelShader(
        L"tone_mapping_pixel_shader.cso",
        L"src\\shaders\\tone_mapping_pixel_shader.hlsl",
        m_toneMappingPixelShader,
        "ToneMappingPixelShader");
}

HRESULT Renderer::CreateGeometry()
{
    const SceneVertex cubeVertices[] =
    {
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
        { XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
        { XMFLOAT3(0.5f,  0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
        { XMFLOAT3(0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
        { XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
        { XMFLOAT3(0.5f, -0.5f,  0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
        { XMFLOAT3(0.5f,  0.5f,  0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
        { XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
        { XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(0.5f,  0.5f,  0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(0.5f,  0.5f, -0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
        { XMFLOAT3(0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
        { XMFLOAT3(0.5f, -0.5f,  0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
        { XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
        { XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(0.5f, -0.5f, -0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(0.5f,  0.5f, -0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(0.5f,  0.5f,  0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(0.5f, -0.5f,  0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
    };

    const UINT cubeIndices[] =
    {
        0, 1, 2, 0, 2, 3,
        4, 5, 6, 4, 6, 7,
        8, 9, 10, 8, 10, 11,
        12, 13, 14, 12, 14, 15,
        16, 17, 18, 16, 18, 19,
        20, 21, 22, 20, 22, 23,
    };

    HRESULT hr = CreateMeshGeometry(
        cubeVertices,
        sizeof(SceneVertex),
        ARRAYSIZE(cubeVertices),
        cubeIndices,
        ARRAYSIZE(cubeIndices),
        m_cubeGeometry,
        "CubeMesh");
    if (FAILED(hr))
    {
        return hr;
    }

    const SceneVertex planeVertices[] =
    {
        { XMFLOAT3(-0.5f, 0.0f, -0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(0.5f, 0.0f, -0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(0.5f, 0.0f, 0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(-0.5f, 0.0f, 0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
    };

    const UINT planeIndices[] =
    {
        0, 1, 2,
        0, 2, 3,
    };

    return CreateMeshGeometry(
        planeVertices,
        sizeof(SceneVertex),
        ARRAYSIZE(planeVertices),
        planeIndices,
        ARRAYSIZE(planeIndices),
        m_planeGeometry,
        "PlaneMesh");
}

void Renderer::CreateSceneObjects()
{
    m_sceneObjects.clear();

    SceneObject floor = {};
    // Use a thick box instead of an infinitely thin plane so the scene remains closed from below.
    floor.meshKind = MeshKind::Cube;
    floor.world = CreateWorldMatrix(XMFLOAT3(14.0f, 1.0f, 14.0f), XMFLOAT3(0.0f, -0.5f, 0.0f));
    floor.albedo = XMFLOAT3(0.55f, 0.57f, 0.63f);
    m_sceneObjects.push_back(floor);

    SceneObject centerBox = {};
    centerBox.meshKind = MeshKind::Cube;
    centerBox.world = CreateWorldMatrix(XMFLOAT3(1.5f, 1.5f, 1.5f), XMFLOAT3(0.0f, 0.25f, 0.0f), 0.4f);
    centerBox.albedo = XMFLOAT3(0.9f, 0.82f, 0.76f);
    m_sceneObjects.push_back(centerBox);

    SceneObject leftBox = {};
    leftBox.meshKind = MeshKind::Cube;
    leftBox.world = CreateWorldMatrix(XMFLOAT3(1.0f, 2.1f, 1.0f), XMFLOAT3(-3.1f, 0.55f, -2.2f), -0.25f);
    leftBox.albedo = XMFLOAT3(0.72f, 0.78f, 0.95f);
    m_sceneObjects.push_back(leftBox);

    SceneObject rightBox = {};
    rightBox.meshKind = MeshKind::Cube;
    rightBox.world = CreateWorldMatrix(XMFLOAT3(1.2f, 0.8f, 1.2f), XMFLOAT3(2.1f, -0.1f, 1.2f), 0.65f);
    rightBox.albedo = XMFLOAT3(0.86f, 0.7f, 0.6f);
    m_sceneObjects.push_back(rightBox);

    SceneObject rearBox = {};
    rearBox.meshKind = MeshKind::Cube;
    rearBox.world = CreateWorldMatrix(XMFLOAT3(2.2f, 0.6f, 2.2f), XMFLOAT3(0.0f, -0.2f, -2.6f), 0.1f);
    rearBox.albedo = XMFLOAT3(0.68f, 0.82f, 0.74f);
    m_sceneObjects.push_back(rearBox);
}

void Renderer::InitializeLights()
{
    m_pointLights[0] = { XMFLOAT3(-2.2f, 1.8f, -0.7f), 6.25f, XMFLOAT3(1.0f, 0.35f, 0.30f), 1.0f };
    m_pointLights[1] = { XMFLOAT3(0.0f, 2.2f, 1.2f), 6.0f, XMFLOAT3(0.30f, 1.0f, 0.40f), 1.0f };
    m_pointLights[2] = { XMFLOAT3(2.2f, 1.8f, -0.5f), 6.25f, XMFLOAT3(0.35f, 0.55f, 1.0f), 1.0f };
}

void Renderer::UpdateWindowTitle()
{
    if (m_hwnd == nullptr)
    {
        return;
    }

    const int intensityValue = static_cast<int>(kLightIntensityLevels[static_cast<size_t>(m_lightIntensityIndex)]);
    const std::wstring title =
        m_title +
        L" | LMB orbit | WASD / Arrows move target | Light intensity " +
        std::to_wstring(intensityValue);

    SetWindowTextW(m_hwnd, title.c_str());
}

void Renderer::Update(float deltaTime)
{
    XMVECTOR movementDirection = XMVectorZero();
    const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR cameraPosition = XMLoadFloat3(&m_cameraPosition);
    const XMVECTOR cameraTarget = XMLoadFloat3(&m_cameraTarget);

    XMVECTOR forward = XMVectorSubtract(cameraTarget, cameraPosition);
    forward = XMVectorSetY(forward, 0.0f);

    if (XMVectorGetX(XMVector3LengthSq(forward)) < 1.0e-6f)
    {
        forward = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    }
    else
    {
        forward = XMVector3Normalize(forward);
    }

    XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, forward));

    if (m_keyStates['W'] || m_keyStates[VK_UP])
    {
        movementDirection = XMVectorAdd(movementDirection, forward);
    }

    if (m_keyStates['S'] || m_keyStates[VK_DOWN])
    {
        movementDirection = XMVectorSubtract(movementDirection, forward);
    }

    if (m_keyStates['D'] || m_keyStates[VK_RIGHT])
    {
        movementDirection = XMVectorAdd(movementDirection, right);
    }

    if (m_keyStates['A'] || m_keyStates[VK_LEFT])
    {
        movementDirection = XMVectorSubtract(movementDirection, right);
    }

    if (XMVectorGetX(XMVector3LengthSq(movementDirection)) > 1.0e-6f)
    {
        const float movementSpeed = 3.5f;
        movementDirection = XMVector3Normalize(movementDirection);

        const XMVECTOR movement = XMVectorScale(movementDirection, movementSpeed * deltaTime);
        const XMVECTOR updatedTarget = XMVectorAdd(cameraTarget, movement);
        XMStoreFloat3(&m_cameraTarget, updatedTarget);
    }

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
    const XMMATRIX projectionMatrix = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), aspectRatio, 0.1f, 100.0f);

    XMStoreFloat4x4(&m_viewMatrix, viewMatrix);
    XMStoreFloat4x4(&m_projectionMatrix, projectionMatrix);
}

void Renderer::RenderSceneToHdr()
{
    BeginEvent(L"ScenePass");

    ID3D11RenderTargetView* renderTargets[] = { m_hdrSceneTexture.renderTargetView.Get() };
    m_deviceContext->OMSetRenderTargets(1, renderTargets, m_depthStencilView.Get());
    m_deviceContext->RSSetState(m_rasterizerState.Get());
    m_deviceContext->OMSetDepthStencilState(m_depthStencilState.Get(), 0);
    SetViewport(m_hdrSceneTexture.width, m_hdrSceneTexture.height);

    const float clearColor[4] = { 0.02f, 0.02f, 0.025f, 1.0f };
    m_deviceContext->ClearRenderTargetView(m_hdrSceneTexture.renderTargetView.Get(), clearColor);
    m_deviceContext->ClearDepthStencilView(m_depthStencilView.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    m_deviceContext->IASetInputLayout(m_sceneInputLayout.Get());
    m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_deviceContext->VSSetShader(m_sceneVertexShader.Get(), nullptr, 0);
    m_deviceContext->PSSetShader(m_scenePixelShader.Get(), nullptr, 0);

    const XMMATRIX viewProjection = XMLoadFloat4x4(&m_viewMatrix) * XMLoadFloat4x4(&m_projectionMatrix);

    SceneFrameConstants frameConstants = {};
    frameConstants.viewProjection = StoreMatrix(viewProjection);
    frameConstants.cameraPosition = XMFLOAT4(m_cameraPosition.x, m_cameraPosition.y, m_cameraPosition.z, 1.0f);

    const float selectedIntensity = kLightIntensityLevels[static_cast<size_t>(m_lightIntensityIndex)];
    for (size_t lightIndex = 0; lightIndex < m_pointLights.size(); ++lightIndex)
    {
        const PointLight& light = m_pointLights[lightIndex];
        frameConstants.pointLights[lightIndex].positionRadius =
            XMFLOAT4(light.position.x, light.position.y, light.position.z, light.radius);
        frameConstants.pointLights[lightIndex].colorIntensity =
            XMFLOAT4(light.color.x, light.color.y, light.color.z, light.intensity * selectedIntensity);
    }

    frameConstants.globalParameters = XMFLOAT4(kAmbientStrength, 0.0f, 0.0f, 0.0f);
    UpdateConstantBuffer(m_deviceContext.Get(), m_sceneFrameConstantBuffer, frameConstants);

    ID3D11Buffer* frameBuffers[] = { m_sceneFrameConstantBuffer.Get() };
    ID3D11Buffer* objectBuffers[] = { m_sceneObjectConstantBuffer.Get() };
    m_deviceContext->VSSetConstantBuffers(0, 1, frameBuffers);
    m_deviceContext->PSSetConstantBuffers(0, 1, frameBuffers);
    m_deviceContext->VSSetConstantBuffers(1, 1, objectBuffers);
    m_deviceContext->PSSetConstantBuffers(1, 1, objectBuffers);

    ID3D11ShaderResourceView* nullSrvs[] = { nullptr, nullptr };
    m_deviceContext->PSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);

    for (const SceneObject& object : m_sceneObjects)
    {
        DrawSceneObject(object);
    }

    EndEvent();
}

void Renderer::DrawSceneObject(const SceneObject& object)
{
    const MeshGeometry& geometry = (object.meshKind == MeshKind::Cube) ? m_cubeGeometry : m_planeGeometry;

    const XMMATRIX worldMatrix = XMLoadFloat4x4(&object.world);
    // This project uses row-vector matrix math, so normals are transformed by the inverse matrix.
    const XMMATRIX normalMatrix = XMMatrixInverse(nullptr, worldMatrix);

    SceneObjectConstants objectConstants = {};
    objectConstants.world = StoreMatrix(worldMatrix);
    objectConstants.normalMatrix = StoreMatrix(normalMatrix);
    objectConstants.albedo = XMFLOAT4(object.albedo.x, object.albedo.y, object.albedo.z, 1.0f);
    UpdateConstantBuffer(m_deviceContext.Get(), m_sceneObjectConstantBuffer, objectConstants);

    const UINT stride = sizeof(SceneVertex);
    const UINT offset = 0;
    ID3D11Buffer* vertexBuffers[] = { geometry.vertexBuffer.Get() };
    m_deviceContext->IASetVertexBuffers(0, 1, vertexBuffers, &stride, &offset);
    m_deviceContext->IASetIndexBuffer(geometry.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
    m_deviceContext->DrawIndexed(geometry.indexCount, 0, 0);
}

void Renderer::ComputeAverageLuminance()
{
    if (m_luminanceChain.empty())
    {
        return;
    }

    BeginEvent(L"LuminanceReduce");

    m_deviceContext->IASetInputLayout(nullptr);
    m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_deviceContext->VSSetShader(m_fullscreenVertexShader.Get(), nullptr, 0);
    m_deviceContext->OMSetDepthStencilState(nullptr, 0);

    ID3D11SamplerState* pointSamplers[] = { m_pointClampSampler.Get() };
    m_deviceContext->PSSetSamplers(0, 1, pointSamplers);

    DownsampleConstants downsampleConstants = {};
    downsampleConstants.sourceTexelSize =
        XMFLOAT2(1.0f / static_cast<float>(m_hdrSceneTexture.width), 1.0f / static_cast<float>(m_hdrSceneTexture.height));
    UpdateConstantBuffer(m_deviceContext.Get(), m_downsampleConstantBuffer, downsampleConstants);

    ID3D11Buffer* downsampleBuffers[] = { m_downsampleConstantBuffer.Get() };
    m_deviceContext->PSSetConstantBuffers(0, 1, downsampleBuffers);

    m_deviceContext->PSSetShader(m_luminanceInitialPixelShader.Get(), nullptr, 0);

    ID3D11RenderTargetView* initialRenderTarget[] = { m_luminanceChain[0].renderTargetView.Get() };
    m_deviceContext->OMSetRenderTargets(1, initialRenderTarget, nullptr);
    SetViewport(m_luminanceChain[0].width, m_luminanceChain[0].height);

    ID3D11ShaderResourceView* initialSource[] = { m_hdrSceneTexture.shaderResourceView.Get() };
    m_deviceContext->PSSetShaderResources(0, 1, initialSource);
    m_deviceContext->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrvs[] = { nullptr, nullptr };
    m_deviceContext->PSSetShaderResources(0, 1, nullSrvs);

    for (size_t index = 1; index < m_luminanceChain.size(); ++index)
    {
        const RenderTexture& sourceTexture = m_luminanceChain[index - 1];
        const RenderTexture& targetTexture = m_luminanceChain[index];

        downsampleConstants.sourceTexelSize =
            XMFLOAT2(1.0f / static_cast<float>(sourceTexture.width), 1.0f / static_cast<float>(sourceTexture.height));
        UpdateConstantBuffer(m_deviceContext.Get(), m_downsampleConstantBuffer, downsampleConstants);

        m_deviceContext->PSSetShader(m_luminanceReducePixelShader.Get(), nullptr, 0);

        ID3D11RenderTargetView* renderTargets[] = { targetTexture.renderTargetView.Get() };
        m_deviceContext->OMSetRenderTargets(1, renderTargets, nullptr);
        SetViewport(targetTexture.width, targetTexture.height);

        ID3D11ShaderResourceView* shaderResources[] = { sourceTexture.shaderResourceView.Get() };
        m_deviceContext->PSSetShaderResources(0, 1, shaderResources);
        m_deviceContext->Draw(3, 0);
        m_deviceContext->PSSetShaderResources(0, 1, nullSrvs);
    }

    EndEvent();
}

void Renderer::AdaptEye(float deltaTime)
{
    if (m_luminanceChain.empty())
    {
        return;
    }

    BeginEvent(L"EyeAdaptation");

    const UINT nextAdaptedLuminanceIndex = 1u - m_currentAdaptedLuminanceIndex;
    const RenderTexture& previousAdaptedLuminance = m_adaptedLuminanceTextures[m_currentAdaptedLuminanceIndex];
    const RenderTexture& nextAdaptedLuminance = m_adaptedLuminanceTextures[nextAdaptedLuminanceIndex];
    const RenderTexture& averageLuminance = m_luminanceChain.back();

    AdaptationConstants adaptationConstants = {};
    adaptationConstants.deltaTime = deltaTime;
    adaptationConstants.adaptationRate = kAdaptationRate;
    adaptationConstants.minLuminance = kMinLuminance;
    UpdateConstantBuffer(m_deviceContext.Get(), m_adaptationConstantBuffer, adaptationConstants);

    ID3D11Buffer* adaptationBuffers[] = { m_adaptationConstantBuffer.Get() };
    m_deviceContext->PSSetConstantBuffers(0, 1, adaptationBuffers);

    ID3D11SamplerState* pointSamplers[] = { m_pointClampSampler.Get() };
    m_deviceContext->PSSetSamplers(0, 1, pointSamplers);

    ID3D11RenderTargetView* renderTargets[] = { nextAdaptedLuminance.renderTargetView.Get() };
    m_deviceContext->OMSetRenderTargets(1, renderTargets, nullptr);
    SetViewport(1, 1);

    ID3D11ShaderResourceView* shaderResources[] =
    {
        averageLuminance.shaderResourceView.Get(),
        previousAdaptedLuminance.shaderResourceView.Get(),
    };

    m_deviceContext->IASetInputLayout(nullptr);
    m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_deviceContext->VSSetShader(m_fullscreenVertexShader.Get(), nullptr, 0);
    m_deviceContext->PSSetShader(m_luminanceAdaptPixelShader.Get(), nullptr, 0);
    m_deviceContext->PSSetShaderResources(0, ARRAYSIZE(shaderResources), shaderResources);
    m_deviceContext->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrvs[] = { nullptr, nullptr };
    m_deviceContext->PSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);

    m_currentAdaptedLuminanceIndex = nextAdaptedLuminanceIndex;
    EndEvent();
}

void Renderer::ToneMapToBackBuffer()
{
    BeginEvent(L"ToneMapping");

    ToneMappingConstants toneMappingConstants = {};
    toneMappingConstants.middleGray = kMiddleGray;
    toneMappingConstants.whitePoint = kWhitePoint;
    toneMappingConstants.minLuminance = kMinLuminance;
    UpdateConstantBuffer(m_deviceContext.Get(), m_toneMappingConstantBuffer, toneMappingConstants);

    ID3D11Buffer* toneMappingBuffers[] = { m_toneMappingConstantBuffer.Get() };
    m_deviceContext->PSSetConstantBuffers(0, 1, toneMappingBuffers);

    ID3D11RenderTargetView* renderTargets[] = { m_renderTargetView.Get() };
    m_deviceContext->OMSetRenderTargets(1, renderTargets, nullptr);
    SetViewport(m_width, m_height);

    ID3D11SamplerState* samplers[] = { m_linearClampSampler.Get(), m_pointClampSampler.Get() };
    m_deviceContext->PSSetSamplers(0, ARRAYSIZE(samplers), samplers);

    ID3D11ShaderResourceView* shaderResources[] =
    {
        m_hdrSceneTexture.shaderResourceView.Get(),
        m_adaptedLuminanceTextures[m_currentAdaptedLuminanceIndex].shaderResourceView.Get(),
    };

    m_deviceContext->IASetInputLayout(nullptr);
    m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_deviceContext->VSSetShader(m_fullscreenVertexShader.Get(), nullptr, 0);
    m_deviceContext->PSSetShader(m_toneMappingPixelShader.Get(), nullptr, 0);
    m_deviceContext->PSSetShaderResources(0, ARRAYSIZE(shaderResources), shaderResources);
    m_deviceContext->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrvs[] = { nullptr, nullptr };
    m_deviceContext->PSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);

    EndEvent();
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
        sourceShaderCandidates.emplace_back(currentDirectory / L"hdr-scene" / sourceRelativePath);
    }

    sourceShaderCandidates.emplace_back(executableDirectory / sourceRelativePath);

    const fs::path twoLevelsUp = executableDirectory.parent_path().parent_path();
    if (!twoLevelsUp.empty())
    {
        sourceShaderCandidates.emplace_back(twoLevelsUp / sourceRelativePath);
        sourceShaderCandidates.emplace_back(twoLevelsUp / L"hdr-scene" / sourceRelativePath);
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

HRESULT Renderer::CreateRenderTexture(UINT width, UINT height, DXGI_FORMAT format, RenderTexture& texture,
    const char* debugName)
{
    texture = {};
    texture.width = width;
    texture.height = height;

    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = format;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = m_device->CreateTexture2D(&textureDesc, nullptr, texture.texture.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = m_device->CreateRenderTargetView(texture.texture.Get(), nullptr, texture.renderTargetView.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = m_device->CreateShaderResourceView(texture.texture.Get(), nullptr, texture.shaderResourceView.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    SetDebugName(texture.texture.Get(), debugName);
    SetDebugName(texture.renderTargetView.Get(), std::string(debugName) + "RTV");
    SetDebugName(texture.shaderResourceView.Get(), std::string(debugName) + "SRV");
    return S_OK;
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

    const auto currentTime = std::chrono::steady_clock::now();
    float deltaTime = std::chrono::duration<float>(currentTime - m_previousFrameTime).count();
    m_previousFrameTime = currentTime;
    deltaTime = std::clamp(deltaTime, 0.0001f, 0.1f);

    Update(deltaTime);

    BeginEvent(L"Frame");
    RenderSceneToHdr();
    ComputeAverageLuminance();
    AdaptEye(deltaTime);
    ToneMapToBackBuffer();

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

    m_cubeGeometry = {};
    m_planeGeometry = {};
    m_sceneFrameConstantBuffer.Reset();
    m_sceneObjectConstantBuffer.Reset();
    m_downsampleConstantBuffer.Reset();
    m_adaptationConstantBuffer.Reset();
    m_toneMappingConstantBuffer.Reset();
    m_sceneInputLayout.Reset();
    m_sceneVertexShader.Reset();
    m_scenePixelShader.Reset();
    m_fullscreenVertexShader.Reset();
    m_luminanceInitialPixelShader.Reset();
    m_luminanceReducePixelShader.Reset();
    m_luminanceAdaptPixelShader.Reset();
    m_toneMappingPixelShader.Reset();
    m_pointClampSampler.Reset();
    m_linearClampSampler.Reset();
    m_depthStencilState.Reset();
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

    case WM_KEYDOWN:
        if (wParam < m_keyStates.size())
        {
            m_keyStates[static_cast<size_t>(wParam)] = true;
        }

        if (wParam == '1')
        {
            m_lightIntensityIndex = 0;
            UpdateWindowTitle();
        }
        else if (wParam == '2')
        {
            m_lightIntensityIndex = 1;
            UpdateWindowTitle();
        }
        else if (wParam == '3')
        {
            m_lightIntensityIndex = 2;
            UpdateWindowTitle();
        }

        return 0;

    case WM_KEYUP:
        if (wParam < m_keyStates.size())
        {
            m_keyStates[static_cast<size_t>(wParam)] = false;
        }
        return 0;

    case WM_KILLFOCUS:
        m_keyStates.fill(false);
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
