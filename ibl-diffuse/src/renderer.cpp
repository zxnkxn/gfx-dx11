#include "renderer.h"
#include "hdri_loader.h"

#include <d3d11sdklayers.h>
#include <wincodec.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <sstream>
#include <string_view>
#include <system_error>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
    namespace fs = std::filesystem;

    using SceneVertex = Gltf::Vertex;

    constexpr float kPi = 3.1415926535f;
    constexpr UINT kSphereLatitudeSegments = 32;
    constexpr UINT kSphereLongitudeSegments = 64;
    constexpr UINT kEnvironmentSphereLatitudeSegments = 96;
    constexpr UINT kEnvironmentSphereLongitudeSegments = 192;
    constexpr UINT kEnvironmentCubemapSize = 1024;
    constexpr UINT kEnvironmentCubemapMipLevels = 11;
    constexpr UINT kIrradianceCubemapSize = 32;
    constexpr UINT kPrefilteredEnvironmentCubemapSize = 128;
    constexpr UINT kPrefilteredEnvironmentCubemapMipLevels = 5;
    constexpr UINT kBrdfIntegrationMapSize = 512;
    constexpr float kEnvironmentSphereScale = 80.0f;
    constexpr float kEnvironmentIntensity = 0.08f;
    constexpr float kTargetSceneRadius = 2.5f;
    constexpr float kMaxReflectionLod = static_cast<float>(kPrefilteredEnvironmentCubemapMipLevels - 1u);
    constexpr float kLightMarkerScale = 0.18f;
    constexpr UINT kBloomBlurPairCount = 5;

    struct CubemapCaptureFace
    {
        XMFLOAT3 forward;
        XMFLOAT3 right;
        XMFLOAT3 up;
    };

    constexpr std::array<CubemapCaptureFace, 6> kCubemapCaptureFaces =
    {
        CubemapCaptureFace{ XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        CubemapCaptureFace{ XMFLOAT3(-1.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        CubemapCaptureFace{ XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
        CubemapCaptureFace{ XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
        CubemapCaptureFace{ XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        CubemapCaptureFace{ XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT3(-1.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
    };

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

    void AppendHdrFilesFromDirectory(const fs::path& directory, std::vector<fs::path>& files)
    {
        std::error_code errorCode;
        if (!fs::is_directory(directory, errorCode))
        {
            return;
        }

        for (const fs::directory_entry& entry : fs::directory_iterator(directory, errorCode))
        {
            if (errorCode)
            {
                break;
            }

            if (!entry.is_regular_file(errorCode))
            {
                continue;
            }

            const fs::path filePath = entry.path();
            if (_wcsicmp(filePath.extension().c_str(), L".hdr") == 0)
            {
                files.push_back(filePath);
            }
        }
    }

    void AppendSceneFilesFromDirectory(const fs::path& directory, std::vector<fs::path>& files)
    {
        std::error_code errorCode;
        if (!fs::is_directory(directory, errorCode))
        {
            return;
        }

        for (const fs::directory_entry& entry : fs::directory_iterator(directory, errorCode))
        {
            if (errorCode)
            {
                break;
            }

            if (!entry.is_regular_file(errorCode))
            {
                continue;
            }

            const fs::path filePath = entry.path();
            if (_wcsicmp(filePath.extension().c_str(), L".gltf") == 0 ||
                _wcsicmp(filePath.extension().c_str(), L".glb") == 0)
            {
                files.push_back(filePath);
            }
        }
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

    std::wstring HResultToHex(HRESULT hr)
    {
        std::wstringstream stream;
        stream << L"0x" << std::hex << std::uppercase << static_cast<std::uint32_t>(hr);
        return stream.str();
    }

    void NormalizeSceneForViewing(Gltf::Scene& scene)
    {
        const XMVECTOR boundsMin = XMLoadFloat3(&scene.boundsMin);
        const XMVECTOR boundsMax = XMLoadFloat3(&scene.boundsMax);
        const XMVECTOR center = XMVectorScale(XMVectorAdd(boundsMin, boundsMax), 0.5f);
        const XMVECTOR extents = XMVectorScale(XMVectorSubtract(boundsMax, boundsMin), 0.5f);
        const float radius = XMVectorGetX(XMVector3Length(extents));
        if (radius <= 1.0e-4f)
        {
            return;
        }

        const float uniformScale = kTargetSceneRadius / radius;
        XMFLOAT3 centerPoint = {};
        XMStoreFloat3(&centerPoint, center);

        const XMMATRIX normalizationMatrix =
            XMMatrixTranslation(-centerPoint.x, -centerPoint.y, -centerPoint.z) *
            XMMatrixScaling(uniformScale, uniformScale, uniformScale);

        for (Gltf::NodePrimitive& nodePrimitive : scene.nodePrimitives)
        {
            const XMMATRIX worldMatrix = XMLoadFloat4x4(&nodePrimitive.world);
            nodePrimitive.world = StoreMatrix(worldMatrix * normalizationMatrix);
        }

        const XMVECTOR scaledExtents = XMVectorScale(extents, uniformScale);
        XMStoreFloat3(&scene.boundsMin, XMVectorNegate(scaledExtents));
        XMStoreFloat3(&scene.boundsMax, scaledExtents);
    }
}

Renderer::Renderer() :
    m_hwnd(nullptr),
    m_hInstance(nullptr),
    m_width(1400),
    m_height(900),
    m_isMinimized(false),
    m_title(L"glTF Metallic-Roughness Viewer"),
    m_initializationErrorMessage(),
    m_comInitialized(false),
    m_isOrbiting(false),
    m_lastMousePosition{},
    m_cameraTarget(0.0f, 0.0f, 0.0f),
    m_cameraDistance(17.0f),
    m_cameraYaw(0.65f),
    m_cameraPitch(-0.38f),
    m_cameraPosition(0.0f, 0.0f, 0.0f),
    m_displayMode(DisplayMode::Pbr),
    m_pointLightsEnabled(true),
    m_loadedHdriFileName(L"No HDRI"),
    m_loadedSceneFileName(L"No glTF"),
    m_sceneBoundsMin(-1.0f, -1.0f, -1.0f),
    m_sceneBoundsMax(1.0f, 1.0f, 1.0f),
    m_bloomEnabled(true),
    m_bloomIntensity(1.0f),
    m_bloomThreshold(0.55f),
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

const std::wstring& Renderer::GetInitializationErrorMessage() const
{
    return m_initializationErrorMessage;
}

HRESULT Renderer::Initialize(HINSTANCE hInstance, int nCmdShow)
{
    m_hInstance = hInstance;
    m_initializationErrorMessage.clear();

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(comHr))
    {
        m_comInitialized = true;
    }
    else if (comHr != RPC_E_CHANGED_MODE)
    {
        SetInitializationError(L"Failed to initialize COM.\nHRESULT: " + HResultToHex(comHr));
        return comHr;
    }

    HRESULT hr = RegisterWindowClass(hInstance);
    if (FAILED(hr))
    {
        SetInitializationError(L"Failed to register the window class.\nHRESULT: " + HResultToHex(hr));
        return hr;
    }

    hr = CreateAppWindow(hInstance, nCmdShow);
    if (FAILED(hr))
    {
        SetInitializationError(L"Failed to create the application window.\nHRESULT: " + HResultToHex(hr));
        return hr;
    }

    hr = InitializeDirectX();
    if (FAILED(hr))
    {
        return hr;
    }

    hr = LoadSceneModel();
    if (FAILED(hr))
    {
        return hr;
    }

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
    windowClass.lpszClassName = L"GltfViewerWindowClass";

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
        L"GltfViewerWindowClass",
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
        if (m_initializationErrorMessage.empty())
        {
            SetInitializationError(L"Failed to create the Direct3D device/context.\nHRESULT: " + HResultToHex(hr));
        }
        return hr;
    }

    hr = CreateSwapChain();
    if (FAILED(hr))
    {
        if (m_initializationErrorMessage.empty())
        {
            SetInitializationError(L"Failed to create the swap chain.\nHRESULT: " + HResultToHex(hr));
        }
        return hr;
    }

    hr = CreateWindowSizeResources();
    if (FAILED(hr))
    {
        if (m_initializationErrorMessage.empty())
        {
            SetInitializationError(L"Failed to create the window-size dependent render targets.\nHRESULT: " + HResultToHex(hr));
        }
        return hr;
    }

    hr = CreatePipelineStates();
    if (FAILED(hr))
    {
        if (m_initializationErrorMessage.empty())
        {
            SetInitializationError(L"Failed to create Direct3D pipeline states.\nHRESULT: " + HResultToHex(hr));
        }
        return hr;
    }

    hr = CreateSamplers();
    if (FAILED(hr))
    {
        if (m_initializationErrorMessage.empty())
        {
            SetInitializationError(L"Failed to create sampler states.\nHRESULT: " + HResultToHex(hr));
        }
        return hr;
    }

    hr = CreateConstantBuffers();
    if (FAILED(hr))
    {
        if (m_initializationErrorMessage.empty())
        {
            SetInitializationError(L"Failed to create constant buffers.\nHRESULT: " + HResultToHex(hr));
        }
        return hr;
    }

    hr = CreateShaders();
    if (FAILED(hr))
    {
        if (m_initializationErrorMessage.empty())
        {
            SetInitializationError(L"Failed to create shaders or input layout.\nHRESULT: " + HResultToHex(hr));
        }
        return hr;
    }

    hr = CreateGeometry();
    if (FAILED(hr))
    {
        if (m_initializationErrorMessage.empty())
        {
            SetInitializationError(L"Failed to create mesh geometry.\nHRESULT: " + HResultToHex(hr));
        }
        return hr;
    }

    hr = CreateEnvironmentCubemap();
    if (FAILED(hr))
    {
        if (m_initializationErrorMessage.empty())
        {
            SetInitializationError(L"Failed to initialize the environment maps.\nHRESULT: " + HResultToHex(hr));
        }
        return hr;
    }

    return S_OK;
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

    hr = CreateFloatTexture2D(
        m_width,
        m_height,
        m_hdrSceneTexture,
        m_hdrSceneShaderResourceView,
        "GLTFHdrScene");
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateTextureRenderTargetView(
        m_hdrSceneTexture.Get(),
        m_hdrSceneRenderTargetView,
        "GLTFHdrScene.RTV");
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateFloatTexture2D(
        m_width,
        m_height,
        m_bloomSourceTexture,
        m_bloomSourceShaderResourceView,
        "GLTFBloomSource");
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateTextureRenderTargetView(
        m_bloomSourceTexture.Get(),
        m_bloomSourceRenderTargetView,
        "GLTFBloomSource.RTV");
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateFloatTexture2D(
        m_width,
        m_height,
        m_bloomTextureA,
        m_bloomTextureAShaderResourceView,
        "GLTFBloomTextureA");
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateTextureRenderTargetView(
        m_bloomTextureA.Get(),
        m_bloomTextureARenderTargetView,
        "GLTFBloomTextureA.RTV");
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateFloatTexture2D(
        m_width,
        m_height,
        m_bloomTextureB,
        m_bloomTextureBShaderResourceView,
        "GLTFBloomTextureB");
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateTextureRenderTargetView(
        m_bloomTextureB.Get(),
        m_bloomTextureBRenderTargetView,
        "GLTFBloomTextureB.RTV");
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
    m_bloomSourceShaderResourceView.Reset();
    m_bloomSourceRenderTargetView.Reset();
    m_bloomSourceTexture.Reset();
    m_bloomTextureBShaderResourceView.Reset();
    m_bloomTextureBRenderTargetView.Reset();
    m_bloomTextureB.Reset();
    m_bloomTextureAShaderResourceView.Reset();
    m_bloomTextureARenderTargetView.Reset();
    m_bloomTextureA.Reset();
    m_hdrSceneShaderResourceView.Reset();
    m_hdrSceneRenderTargetView.Reset();
    m_hdrSceneTexture.Reset();
    m_renderTargetView.Reset();
    m_depthStencilView.Reset();
    m_depthStencilBuffer.Reset();
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

    D3D11_RASTERIZER_DESC doubleSidedRasterizerDesc = rasterizerDesc;
    doubleSidedRasterizerDesc.CullMode = D3D11_CULL_NONE;

    hr = m_device->CreateRasterizerState(&doubleSidedRasterizerDesc, m_doubleSidedRasterizerState.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_RASTERIZER_DESC skyRasterizerDesc = rasterizerDesc;
    // The camera lives inside the environment sphere, so we must draw only one
    // shell to avoid blending the opposite hemisphere over the visible one.
    skyRasterizerDesc.CullMode = D3D11_CULL_BACK;

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

    D3D11_DEPTH_STENCIL_DESC transparentDepthStencilDesc = depthStencilDesc;
    transparentDepthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;

    hr = m_device->CreateDepthStencilState(&transparentDepthStencilDesc, m_transparentDepthStencilState.ReleaseAndGetAddressOf());
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

    D3D11_BLEND_DESC alphaBlendDesc = {};
    alphaBlendDesc.RenderTarget[0].BlendEnable = TRUE;
    alphaBlendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    alphaBlendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    alphaBlendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    alphaBlendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    alphaBlendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    alphaBlendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    alphaBlendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = m_device->CreateBlendState(&alphaBlendDesc, m_alphaBlendState.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    SetDebugName(m_rasterizerState.Get(), "PBRSphereRasterizerState");
    SetDebugName(m_doubleSidedRasterizerState.Get(), "GLTFDoubleSidedRasterizerState");
    SetDebugName(m_skyRasterizerState.Get(), "PBRSphereSkyRasterizerState");
    SetDebugName(m_depthStencilState.Get(), "PBRSphereDepthStencilState");
    SetDebugName(m_transparentDepthStencilState.Get(), "GLTFTransparentDepthStencilState");
    SetDebugName(m_skyDepthStencilState.Get(), "PBRSphereSkyDepthStencilState");
    SetDebugName(m_alphaBlendState.Get(), "GLTFAlphaBlendState");
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

    D3D11_SAMPLER_DESC wrapSamplerDesc = samplerDesc;
    wrapSamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    wrapSamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    wrapSamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;

    const HRESULT wrapHr = m_device->CreateSamplerState(&wrapSamplerDesc, m_linearWrapSampler.ReleaseAndGetAddressOf());
    if (FAILED(wrapHr))
    {
        return wrapHr;
    }

    SetDebugName(m_linearWrapSampler.Get(), "GLTFLinearWrapSampler");
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

    hr = CreateConstantBuffer<SkyFrameConstants>(m_device.Get(), m_skyFrameConstantBuffer, "PBRSphereSkyFrameCB");
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateConstantBuffer<CaptureConstants>(m_device.Get(), m_captureConstantBuffer, "IBLDiffuseCaptureCB");
    if (FAILED(hr))
    {
        return hr;
    }

    return CreateConstantBuffer<PostProcessConstants>(m_device.Get(), m_postProcessConstantBuffer, "GLTFPostProcessCB");
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
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(SceneVertex, texCoord), D3D11_INPUT_PER_VERTEX_DATA, 0 },
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

    ComPtr<ID3DBlob> captureVertexShaderBlob;
    hr = LoadShaderBlob(
        L"cubemap_capture_vertex_shader.cso",
        L"src\\shaders\\cubemap_capture_vertex_shader.hlsl",
        "VS",
        "vs_5_0",
        captureVertexShaderBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = m_device->CreateVertexShader(
        captureVertexShaderBlob->GetBufferPointer(),
        captureVertexShaderBlob->GetBufferSize(),
        nullptr,
        m_captureVertexShader.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    ComPtr<ID3DBlob> fullscreenVertexShaderBlob;
    hr = LoadShaderBlob(
        L"fullscreen_vertex_shader.cso",
        L"src\\shaders\\fullscreen_vertex_shader.hlsl",
        "VS",
        "vs_5_0",
        fullscreenVertexShaderBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = m_device->CreateVertexShader(
        fullscreenVertexShaderBlob->GetBufferPointer(),
        fullscreenVertexShaderBlob->GetBufferSize(),
        nullptr,
        m_fullscreenVertexShader.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = createPixelShader(
        L"equirectangular_to_cubemap_pixel_shader.cso",
        L"src\\shaders\\equirectangular_to_cubemap_pixel_shader.hlsl",
        "IBLDiffuseEquirectangularToCubemapPixelShader",
        m_equirectangularToCubemapPixelShader.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = createPixelShader(
        L"irradiance_convolution_pixel_shader.cso",
        L"src\\shaders\\irradiance_convolution_pixel_shader.hlsl",
        "IBLDiffuseIrradianceConvolutionPixelShader",
        m_irradianceConvolutionPixelShader.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = createPixelShader(
        L"prefilter_environment_pixel_shader.cso",
        L"src\\shaders\\prefilter_environment_pixel_shader.hlsl",
        "IBLSpecularPrefilterEnvironmentPixelShader",
        m_prefilterEnvironmentPixelShader.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = createPixelShader(
        L"brdf_integration_pixel_shader.cso",
        L"src\\shaders\\brdf_integration_pixel_shader.hlsl",
        "IBLSpecularBrdfIntegrationPixelShader",
        m_brdfIntegrationPixelShader.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = createPixelShader(
        L"bloom_blur_pixel_shader.cso",
        L"src\\shaders\\bloom_blur_pixel_shader.hlsl",
        "GLTFBloomBlurPixelShader",
        m_bloomBlurPixelShader.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = createPixelShader(
        L"bloom_composite_pixel_shader.cso",
        L"src\\shaders\\bloom_composite_pixel_shader.hlsl",
        "GLTFBloomCompositePixelShader",
        m_bloomCompositePixelShader.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    SetDebugName(m_pbrSceneVertexShader.Get(), "PBRSphereSceneVertexShader");
    SetDebugName(m_skyVertexShader.Get(), "PBRSphereSkyVertexShader");
    SetDebugName(m_captureVertexShader.Get(), "IBLDiffuseCaptureVertexShader");
    SetDebugName(m_fullscreenVertexShader.Get(), "IBLSpecularFullscreenVertexShader");
    SetDebugName(m_sceneInputLayout.Get(), "PBRSphereInputLayout");
    return S_OK;
}

HRESULT Renderer::CreateGeometry()
{
    HRESULT hr = CreateSphereMeshGeometry(
        kSphereLatitudeSegments,
        kSphereLongitudeSegments,
        m_sphereGeometry,
        "IBLDiffuseSphereMesh");
    if (FAILED(hr))
    {
        return hr;
    }

    return CreateSphereMeshGeometry(
        kEnvironmentSphereLatitudeSegments,
        kEnvironmentSphereLongitudeSegments,
        m_environmentSphereGeometry,
        "IBLDiffuseEnvironmentSphereMesh");
}

HRESULT Renderer::CreateSphereMeshGeometry(UINT latitudeSegments, UINT longitudeSegments, MeshGeometry& geometry, const char* debugName)
{
    std::vector<SceneVertex> vertices;
    std::vector<UINT> indices;

    vertices.reserve((latitudeSegments + 1) * (longitudeSegments + 1));
    indices.reserve(latitudeSegments * longitudeSegments * 6);

    for (UINT latitude = 0; latitude <= latitudeSegments; ++latitude)
    {
        const float v = static_cast<float>(latitude) / static_cast<float>(latitudeSegments);
        const float phi = v * kPi;
        const float y = std::cos(phi);
        const float ringRadius = std::sin(phi);

        for (UINT longitude = 0; longitude <= longitudeSegments; ++longitude)
        {
            const float u = static_cast<float>(longitude) / static_cast<float>(longitudeSegments);
            const float theta = u * kPi * 2.0f;

            const float x = std::cos(theta) * ringRadius;
            const float z = std::sin(theta) * ringRadius;
            vertices.push_back({ XMFLOAT3(x, y, z), XMFLOAT3(x, y, z), XMFLOAT2(u, 1.0f - v) });
        }
    }

    const UINT stride = longitudeSegments + 1;
    for (UINT latitude = 0; latitude < latitudeSegments; ++latitude)
    {
        for (UINT longitude = 0; longitude < longitudeSegments; ++longitude)
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
        geometry,
        debugName);
}

HRESULT Renderer::LoadSceneModel()
{
    ReleaseModelResources();

    const fs::path sceneFilePath = FindSceneFile();
    if (sceneFilePath.empty())
    {
        m_loadedSceneFileName = L"Missing glTF";
        SetInitializationError(
            L"glTF scene file was not found.\n"
            L"Place a .gltf or .glb file in one of the assets/models folders near the project or executable.");
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    Gltf::Scene scene = {};
    const HRESULT sceneLoadHr = Gltf::LoadScene(sceneFilePath, scene);
    if (FAILED(sceneLoadHr))
    {
        m_loadedSceneFileName = L"Failed glTF";
        std::wstring details = Gltf::GetLastErrorMessage();
        if (details.empty())
        {
            details = L"The loader did not provide additional details.";
        }

        SetInitializationError(
            L"Failed to load glTF scene:\n" +
            sceneFilePath.wstring() +
            L"\nReason: " +
            details +
            L"\nHRESULT: " +
            HResultToHex(sceneLoadHr));
        return sceneLoadHr;
    }

    NormalizeSceneForViewing(scene);

    m_loadedSceneFileName = scene.sourceFileName;
    m_sceneBoundsMin = scene.boundsMin;
    m_sceneBoundsMax = scene.boundsMax;
    m_initializationErrorMessage.clear();

    auto failSceneInitialization = [this, &sceneFilePath](const std::wstring& step, HRESULT error, const std::wstring& details = std::wstring()) -> HRESULT
    {
        std::wstring message = L"Failed while preparing the glTF scene resources.";
        message += L"\nScene: ";
        message += sceneFilePath.wstring();
        message += L"\nStep: ";
        message += step;
        if (!details.empty())
        {
            message += L"\nReason: ";
            message += details;
        }

        message += L"\nHRESULT: ";
        message += HResultToHex(error);
        SetInitializationError(std::move(message));
        return error;
    };

    auto describeTextureStage = [&scene](const wchar_t* textureRole, size_t materialIndex, const Gltf::TextureRef& textureRef) -> std::wstring
    {
        std::wstring step = L"loading ";
        step += textureRole;
        step += L" for material ";
        step += std::to_wstring(materialIndex);

        if (textureRef.imageIndex >= 0 && static_cast<size_t>(textureRef.imageIndex) < scene.images.size())
        {
            const std::wstring& debugName = scene.images[static_cast<size_t>(textureRef.imageIndex)].debugName;
            if (!debugName.empty())
            {
                step += L" (";
                step += debugName;
                step += L")";
            }
        }

        return step;
    };

    HRESULT hr = S_OK;
    if (m_defaultWhiteLinearShaderResourceView == nullptr)
    {
        hr = CreateSolidColorTexture(
            XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DXGI_FORMAT_R8G8B8A8_UNORM,
            m_defaultWhiteLinearShaderResourceView,
            "DefaultWhiteLinear");
        if (FAILED(hr))
        {
            return failSceneInitialization(L"creating the default linear material texture", hr);
        }
    }

    if (m_defaultWhiteSrgbShaderResourceView == nullptr)
    {
        hr = CreateSolidColorTexture(
            XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            m_defaultWhiteSrgbShaderResourceView,
            "DefaultWhiteSrgb");
        if (FAILED(hr))
        {
            return failSceneInitialization(L"creating the default sRGB material texture", hr);
        }
    }

    std::vector<ComPtr<ID3D11ShaderResourceView>> linearTextureCache(scene.images.size());
    std::vector<ComPtr<ID3D11ShaderResourceView>> srgbTextureCache(scene.images.size());

    auto resolveTexture = [this, &scene, &linearTextureCache, &srgbTextureCache](
        const Gltf::TextureRef& textureRef,
        bool srgb,
        const char* debugNamePrefix,
        const ComPtr<ID3D11ShaderResourceView>& defaultTexture,
        ComPtr<ID3D11ShaderResourceView>& destination) -> HRESULT
    {
        if (textureRef.imageIndex < 0)
        {
            destination = defaultTexture;
            return S_OK;
        }

        if (static_cast<size_t>(textureRef.imageIndex) >= scene.images.size())
        {
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }

        std::vector<ComPtr<ID3D11ShaderResourceView>>& cache = srgb ? srgbTextureCache : linearTextureCache;
        ComPtr<ID3D11ShaderResourceView>& cachedTexture = cache[static_cast<size_t>(textureRef.imageIndex)];
        if (cachedTexture == nullptr)
        {
            const HRESULT loadHr = CreateTextureFromEncodedImage(
                scene.images[static_cast<size_t>(textureRef.imageIndex)],
                srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM,
                cachedTexture,
                debugNamePrefix);
            if (FAILED(loadHr))
            {
                return loadHr;
            }
        }

        destination = cachedTexture;
        return S_OK;
    };

    m_modelMaterials.reserve(scene.materials.size());
    for (size_t materialIndex = 0; materialIndex < scene.materials.size(); ++materialIndex)
    {
        const Gltf::Material& sourceMaterial = scene.materials[materialIndex];
        ModelMaterial material = {};
        material.baseColorFactor = sourceMaterial.baseColorFactor;
        material.emissiveFactor = XMFLOAT4(
            sourceMaterial.emissiveFactor.x,
            sourceMaterial.emissiveFactor.y,
            sourceMaterial.emissiveFactor.z,
            1.0f);
        material.roughnessFactor = sourceMaterial.roughnessFactor;
        material.metallicFactor = sourceMaterial.metallicFactor;
        material.occlusionStrength = sourceMaterial.occlusionStrength;
        material.alphaCutoff = sourceMaterial.alphaCutoff;
        material.doubleSided = sourceMaterial.doubleSided;
        material.alphaMode = sourceMaterial.alphaMode;
        material.baseColorTextureShaderResourceView = m_defaultWhiteSrgbShaderResourceView;
        material.metallicRoughnessTextureShaderResourceView = m_defaultWhiteLinearShaderResourceView;
        material.emissiveTextureShaderResourceView = m_defaultWhiteSrgbShaderResourceView;
        material.occlusionTextureShaderResourceView = m_defaultWhiteLinearShaderResourceView;

        hr = resolveTexture(
            sourceMaterial.baseColorTexture,
            true,
            "GLTFBaseColorTexture",
            m_defaultWhiteSrgbShaderResourceView,
            material.baseColorTextureShaderResourceView);
        if (FAILED(hr))
        {
            ReleaseModelResources();
            return failSceneInitialization(
                describeTextureStage(L"the base color texture", materialIndex, sourceMaterial.baseColorTexture),
                hr);
        }

        hr = resolveTexture(
            sourceMaterial.metallicRoughnessTexture,
            false,
            "GLTFMetallicRoughnessTexture",
            m_defaultWhiteLinearShaderResourceView,
            material.metallicRoughnessTextureShaderResourceView);
        if (FAILED(hr))
        {
            ReleaseModelResources();
            return failSceneInitialization(
                describeTextureStage(L"the metallic-roughness texture", materialIndex, sourceMaterial.metallicRoughnessTexture),
                hr);
        }

        hr = resolveTexture(
            sourceMaterial.emissiveTexture,
            true,
            "GLTFEmissiveTexture",
            m_defaultWhiteSrgbShaderResourceView,
            material.emissiveTextureShaderResourceView);
        if (FAILED(hr))
        {
            ReleaseModelResources();
            return failSceneInitialization(
                describeTextureStage(L"the emissive texture", materialIndex, sourceMaterial.emissiveTexture),
                hr);
        }

        hr = resolveTexture(
            sourceMaterial.occlusionTexture,
            false,
            "GLTFOcclusionTexture",
            m_defaultWhiteLinearShaderResourceView,
            material.occlusionTextureShaderResourceView);
        if (FAILED(hr))
        {
            ReleaseModelResources();
            return failSceneInitialization(
                describeTextureStage(L"the occlusion texture", materialIndex, sourceMaterial.occlusionTexture),
                hr);
        }

        m_modelMaterials.push_back(material);
    }

    m_modelPrimitives.reserve(scene.primitives.size());
    for (size_t primitiveIndex = 0; primitiveIndex < scene.primitives.size(); ++primitiveIndex)
    {
        const Gltf::Primitive& sourcePrimitive = scene.primitives[primitiveIndex];

        MeshGeometry geometry = {};
        const std::string debugName = "GLTFPrimitive" + std::to_string(primitiveIndex);
        const HRESULT geometryHr = CreateMeshGeometry(
            sourcePrimitive.vertices.data(),
            sizeof(SceneVertex),
            static_cast<UINT>(sourcePrimitive.vertices.size()),
            sourcePrimitive.indices.data(),
            static_cast<UINT>(sourcePrimitive.indices.size()),
            geometry,
            debugName.c_str());
        if (FAILED(geometryHr))
        {
            ReleaseModelResources();
            return failSceneInitialization(
                L"creating mesh buffers for primitive " + std::to_wstring(primitiveIndex),
                geometryHr);
        }

        ModelPrimitive primitive = {};
        primitive.geometry = std::move(geometry);
        primitive.materialIndex =
            (sourcePrimitive.materialIndex < m_modelMaterials.size())
            ? sourcePrimitive.materialIndex
            : 0u;
        m_modelPrimitives.push_back(std::move(primitive));
    }

    m_modelDrawItems.reserve(scene.nodePrimitives.size());
    for (const Gltf::NodePrimitive& sourceDrawItem : scene.nodePrimitives)
    {
        if (sourceDrawItem.primitiveIndex >= m_modelPrimitives.size())
        {
            ReleaseModelResources();
            return failSceneInitialization(
                L"validating node draw item references",
                HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
                L"A node references a primitive index that was not loaded.");
        }

        ModelDrawItem drawItem = {};
        drawItem.primitiveIndex = sourceDrawItem.primitiveIndex;
        drawItem.world = sourceDrawItem.world;
        m_modelDrawItems.push_back(drawItem);
    }

    if (m_modelDrawItems.empty())
    {
        return failSceneInitialization(
            L"building the draw list",
            HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
            L"The glTF scene did not produce any drawable primitives.");
    }

    return S_OK;
}

HRESULT Renderer::CreateSolidColorTexture(
    const XMFLOAT4& color,
    DXGI_FORMAT shaderResourceFormat,
    ComPtr<ID3D11ShaderResourceView>& shaderResourceView,
    const char* debugNamePrefix)
{
    const auto toByte = [](float value) -> std::uint8_t
        {
            return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
        };

    const std::uint8_t pixel[] =
    {
        toByte(color.x),
        toByte(color.y),
        toByte(color.z),
        toByte(color.w),
    };

    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = 1;
    textureDesc.Height = 1;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = shaderResourceFormat;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initialData = {};
    initialData.pSysMem = pixel;
    initialData.SysMemPitch = sizeof(pixel);

    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = m_device->CreateTexture2D(&textureDesc, &initialData, texture.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc = {};
    shaderResourceViewDesc.Format = shaderResourceFormat;
    shaderResourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    shaderResourceViewDesc.Texture2D.MipLevels = 1;

    hr = m_device->CreateShaderResourceView(texture.Get(), &shaderResourceViewDesc, shaderResourceView.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    SetDebugName(texture.Get(), std::string(debugNamePrefix) + ".Texture");
    SetDebugName(shaderResourceView.Get(), std::string(debugNamePrefix) + ".SRV");
    return S_OK;
}

HRESULT Renderer::CreateTextureFromEncodedImage(
    const Gltf::Image& image,
    DXGI_FORMAT shaderResourceFormat,
    ComPtr<ID3D11ShaderResourceView>& shaderResourceView,
    const char* debugNamePrefix)
{
    if (image.encodedBytes.empty())
    {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    ComPtr<IWICImagingFactory> imagingFactory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(imagingFactory.ReleaseAndGetAddressOf()));
    if (FAILED(hr))
    {
        return hr;
    }

    ComPtr<IWICStream> stream;
    hr = imagingFactory->CreateStream(stream.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = stream->InitializeFromMemory(
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(image.encodedBytes.data())),
        static_cast<DWORD>(image.encodedBytes.size()));
    if (FAILED(hr))
    {
        return hr;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = imagingFactory->CreateDecoderFromStream(
        stream.Get(),
        nullptr,
        WICDecodeMetadataCacheOnDemand,
        decoder.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    ComPtr<IWICFormatConverter> formatConverter;
    hr = imagingFactory->CreateFormatConverter(formatConverter.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    hr = formatConverter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
    {
        return hr;
    }

    UINT width = 0;
    UINT height = 0;
    hr = formatConverter->GetSize(&width, &height);
    if (FAILED(hr))
    {
        return hr;
    }

    if (width == 0 || height == 0)
    {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    const UINT rowPitch = width * 4u;
    std::vector<std::uint8_t> pixels(static_cast<size_t>(rowPitch) * static_cast<size_t>(height));
    hr = formatConverter->CopyPixels(nullptr, rowPitch, static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = shaderResourceFormat;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA subresourceData = {};
    subresourceData.pSysMem = pixels.data();
    subresourceData.SysMemPitch = rowPitch;

    ComPtr<ID3D11Texture2D> texture;
    hr = m_device->CreateTexture2D(&textureDesc, &subresourceData, texture.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc = {};
    shaderResourceViewDesc.Format = shaderResourceFormat;
    shaderResourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    shaderResourceViewDesc.Texture2D.MipLevels = 1;

    hr = m_device->CreateShaderResourceView(texture.Get(), &shaderResourceViewDesc, shaderResourceView.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    SetDebugName(texture.Get(), std::string(debugNamePrefix) + ".Texture");
    SetDebugName(shaderResourceView.Get(), std::string(debugNamePrefix) + ".SRV");
    return S_OK;
}

HRESULT Renderer::CreateEnvironmentCubemap()
{
    m_hdriTexture.Reset();
    m_hdriTextureShaderResourceView.Reset();
    m_environmentCubemap.Reset();
    m_environmentCubemapShaderResourceView.Reset();
    m_irradianceCubemap.Reset();
    m_irradianceCubemapShaderResourceView.Reset();
    m_prefilteredEnvironmentCubemap.Reset();
    m_prefilteredEnvironmentCubemapShaderResourceView.Reset();
    m_brdfIntegrationTexture.Reset();
    m_brdfIntegrationShaderResourceView.Reset();

    const fs::path hdriFilePath = FindHdriFile();
    if (hdriFilePath.empty())
    {
        m_loadedHdriFileName = L"Missing HDRI";
        SetInitializationError(
            L"HDRI file was not found.\n"
            L"Place a Radiance .hdr file in one of the assets/hdri folders near the project or executable.");
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    HdriImage hdriImage = {};
    HRESULT hr = LoadRadianceHdrImage(hdriFilePath, hdriImage);
    if (FAILED(hr))
    {
        m_loadedHdriFileName = L"Invalid HDRI";
        SetInitializationError(
            L"Failed to read HDRI file:\n" +
            hdriFilePath.wstring() +
            L"\nHRESULT: " +
            HResultToHex(hr));
        return hr;
    }

    hr = CreateHdriTexture(hdriImage);
    if (FAILED(hr))
    {
        SetInitializationError(
            L"Failed to create the GPU texture from the HDRI:\n" +
            hdriFilePath.wstring() +
            L"\nHRESULT: " +
            HResultToHex(hr));
        return hr;
    }

    hr = CreateFloatCubemap(
        kEnvironmentCubemapSize,
        kEnvironmentCubemapMipLevels,
        true,
        m_environmentCubemap,
        m_environmentCubemapShaderResourceView,
        "IBLDiffuseEnvironmentCubemap");
    if (FAILED(hr))
    {
        SetInitializationError(L"Failed to allocate the environment cubemap.\nHRESULT: " + HResultToHex(hr));
        return hr;
    }

    hr = RenderCubemapFaces(
        m_environmentCubemap.Get(),
        kEnvironmentCubemapSize,
        0,
        0.0f,
        0.0f,
        m_equirectangularToCubemapPixelShader.Get(),
        m_hdriTextureShaderResourceView.Get(),
        L"HdriToCubemapPass");
    if (FAILED(hr))
    {
        SetInitializationError(L"Failed to render the environment cubemap from the HDRI.\nHRESULT: " + HResultToHex(hr));
        return hr;
    }

    m_deviceContext->GenerateMips(m_environmentCubemapShaderResourceView.Get());

    hr = CreateIrradianceMap();
    if (FAILED(hr))
    {
        SetInitializationError(L"Failed to build the irradiance cubemap.\nHRESULT: " + HResultToHex(hr));
        return hr;
    }

    hr = CreatePrefilteredEnvironmentMap();
    if (FAILED(hr))
    {
        SetInitializationError(L"Failed to build the prefiltered environment cubemap.\nHRESULT: " + HResultToHex(hr));
        return hr;
    }

    hr = CreateBrdfIntegrationMap();
    if (FAILED(hr))
    {
        SetInitializationError(L"Failed to build the BRDF integration LUT.\nHRESULT: " + HResultToHex(hr));
        return hr;
    }

    m_loadedHdriFileName = hdriFilePath.filename().wstring();
    m_initializationErrorMessage.clear();
    return S_OK;
}

HRESULT Renderer::CreateHdriTexture(const HdriImage& image)
{
    if (image.width == 0 || image.height == 0 || image.pixels.empty())
    {
        return E_INVALIDARG;
    }

    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = image.width;
    textureDesc.Height = image.height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initialData = {};
    initialData.pSysMem = image.pixels.data();
    initialData.SysMemPitch = image.width * sizeof(float) * 4u;

    HRESULT hr = m_device->CreateTexture2D(&textureDesc, &initialData, m_hdriTexture.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc = {};
    shaderResourceViewDesc.Format = textureDesc.Format;
    shaderResourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    shaderResourceViewDesc.Texture2D.MipLevels = 1;
    shaderResourceViewDesc.Texture2D.MostDetailedMip = 0;

    hr = m_device->CreateShaderResourceView(
        m_hdriTexture.Get(),
        &shaderResourceViewDesc,
        m_hdriTextureShaderResourceView.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    SetDebugName(m_hdriTexture.Get(), "IBLDiffuseHdriTexture");
    SetDebugName(m_hdriTextureShaderResourceView.Get(), "IBLDiffuseHdriTextureSRV");
    return S_OK;
}

HRESULT Renderer::CreateFloatCubemap(
    UINT faceSize,
    UINT mipLevels,
    bool generateMips,
    ComPtr<ID3D11Texture2D>& texture,
    ComPtr<ID3D11ShaderResourceView>& shaderResourceView,
    const char* debugNamePrefix)
{
    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = faceSize;
    textureDesc.Height = faceSize;
    textureDesc.MipLevels = mipLevels;
    textureDesc.ArraySize = 6;
    textureDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    textureDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE | (generateMips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0u);

    HRESULT hr = m_device->CreateTexture2D(&textureDesc, nullptr, texture.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc = {};
    shaderResourceViewDesc.Format = textureDesc.Format;
    shaderResourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    shaderResourceViewDesc.TextureCube.MostDetailedMip = 0;
    shaderResourceViewDesc.TextureCube.MipLevels = mipLevels;

    hr = m_device->CreateShaderResourceView(
        texture.Get(),
        &shaderResourceViewDesc,
        shaderResourceView.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    SetDebugName(texture.Get(), std::string(debugNamePrefix));
    SetDebugName(shaderResourceView.Get(), std::string(debugNamePrefix) + ".SRV");
    return S_OK;
}

HRESULT Renderer::RenderCubemapFaces(
    ID3D11Texture2D* targetTexture,
    UINT faceSize,
    UINT mipLevel,
    float roughness,
    float sourceCubemapFaceSize,
    ID3D11PixelShader* pixelShader,
    ID3D11ShaderResourceView* sourceShaderResourceView,
    const wchar_t* eventName)
{
    if (targetTexture == nullptr || pixelShader == nullptr || sourceShaderResourceView == nullptr)
    {
        return E_POINTER;
    }

    std::array<ComPtr<ID3D11RenderTargetView>, 6> faceRenderTargetViews;
    for (UINT faceIndex = 0; faceIndex < 6; ++faceIndex)
    {
        D3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc = {};
        renderTargetViewDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        renderTargetViewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        renderTargetViewDesc.Texture2DArray.MipSlice = mipLevel;
        renderTargetViewDesc.Texture2DArray.FirstArraySlice = faceIndex;
        renderTargetViewDesc.Texture2DArray.ArraySize = 1;

        const HRESULT hr = m_device->CreateRenderTargetView(
            targetTexture,
            &renderTargetViewDesc,
            faceRenderTargetViews[faceIndex].ReleaseAndGetAddressOf());
        if (FAILED(hr))
        {
            return hr;
        }
    }

    BeginEvent(eventName);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<FLOAT>(faceSize);
    viewport.Height = static_cast<FLOAT>(faceSize);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_deviceContext->RSSetViewports(1, &viewport);

    m_deviceContext->IASetInputLayout(nullptr);
    m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_deviceContext->RSSetState(m_rasterizerState.Get());
    m_deviceContext->OMSetDepthStencilState(m_skyDepthStencilState.Get(), 0);
    m_deviceContext->VSSetShader(m_captureVertexShader.Get(), nullptr, 0);
    m_deviceContext->PSSetShader(pixelShader, nullptr, 0);

    ID3D11Buffer* captureBuffers[] = { m_captureConstantBuffer.Get() };
    m_deviceContext->VSSetConstantBuffers(0, 1, captureBuffers);
    m_deviceContext->PSSetConstantBuffers(0, 1, captureBuffers);

    ID3D11SamplerState* samplers[] = { m_linearClampSampler.Get() };
    m_deviceContext->PSSetSamplers(0, 1, samplers);

    ID3D11ShaderResourceView* shaderResources[] = { sourceShaderResourceView };
    m_deviceContext->PSSetShaderResources(0, 1, shaderResources);

    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    for (UINT faceIndex = 0; faceIndex < 6; ++faceIndex)
    {
        const CubemapCaptureFace& face = kCubemapCaptureFaces[faceIndex];

        CaptureConstants captureConstants = {};
        captureConstants.faceForward = XMFLOAT4(face.forward.x, face.forward.y, face.forward.z, 0.0f);
        captureConstants.faceRight = XMFLOAT4(face.right.x, face.right.y, face.right.z, 0.0f);
        captureConstants.faceUp = XMFLOAT4(face.up.x, face.up.y, face.up.z, 0.0f);
        captureConstants.prefilterParameters = XMFLOAT4(roughness, sourceCubemapFaceSize, 0.0f, 0.0f);
        UpdateConstantBuffer(m_deviceContext.Get(), m_captureConstantBuffer, captureConstants);

        ID3D11RenderTargetView* renderTargets[] = { faceRenderTargetViews[faceIndex].Get() };
        m_deviceContext->OMSetRenderTargets(1, renderTargets, nullptr);
        m_deviceContext->ClearRenderTargetView(faceRenderTargetViews[faceIndex].Get(), clearColor);
        m_deviceContext->Draw(6, 0);
    }

    ID3D11ShaderResourceView* nullShaderResources[] = { nullptr };
    m_deviceContext->PSSetShaderResources(0, 1, nullShaderResources);
    m_deviceContext->OMSetRenderTargets(0, nullptr, nullptr);

    EndEvent();
    return S_OK;
}

HRESULT Renderer::CreateIrradianceMap()
{
    HRESULT hr = CreateFloatCubemap(
        kIrradianceCubemapSize,
        1,
        false,
        m_irradianceCubemap,
        m_irradianceCubemapShaderResourceView,
        "IBLDiffuseIrradianceCubemap");
    if (FAILED(hr))
    {
        return hr;
    }

    return RenderCubemapFaces(
        m_irradianceCubemap.Get(),
        kIrradianceCubemapSize,
        0,
        0.0f,
        0.0f,
        m_irradianceConvolutionPixelShader.Get(),
        m_environmentCubemapShaderResourceView.Get(),
        L"IrradianceConvolutionPass");
}

HRESULT Renderer::CreatePrefilteredEnvironmentMap()
{
    HRESULT hr = CreateFloatCubemap(
        kPrefilteredEnvironmentCubemapSize,
        kPrefilteredEnvironmentCubemapMipLevels,
        false,
        m_prefilteredEnvironmentCubemap,
        m_prefilteredEnvironmentCubemapShaderResourceView,
        "IBLSpecularPrefilteredEnvironmentCubemap");
    if (FAILED(hr))
    {
        return hr;
    }

    for (UINT mipLevel = 0; mipLevel < kPrefilteredEnvironmentCubemapMipLevels; ++mipLevel)
    {
        const UINT mipFaceSize = std::max(kPrefilteredEnvironmentCubemapSize >> mipLevel, 1u);
        const float roughness =
            (kPrefilteredEnvironmentCubemapMipLevels > 1)
            ? (static_cast<float>(mipLevel) / static_cast<float>(kPrefilteredEnvironmentCubemapMipLevels - 1u))
            : 0.0f;

        hr = RenderCubemapFaces(
            m_prefilteredEnvironmentCubemap.Get(),
            mipFaceSize,
            mipLevel,
            roughness,
            static_cast<float>(kEnvironmentCubemapSize),
            m_prefilterEnvironmentPixelShader.Get(),
            m_environmentCubemapShaderResourceView.Get(),
            L"PrefilterEnvironmentPass");
        if (FAILED(hr))
        {
            return hr;
        }
    }

    return S_OK;
}

HRESULT Renderer::CreateFloatTexture2D(
    UINT width,
    UINT height,
    ComPtr<ID3D11Texture2D>& texture,
    ComPtr<ID3D11ShaderResourceView>& shaderResourceView,
    const char* debugNamePrefix)
{
    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    HRESULT hr = m_device->CreateTexture2D(&textureDesc, nullptr, texture.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc = {};
    shaderResourceViewDesc.Format = textureDesc.Format;
    shaderResourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    shaderResourceViewDesc.Texture2D.MostDetailedMip = 0;
    shaderResourceViewDesc.Texture2D.MipLevels = 1;

    hr = m_device->CreateShaderResourceView(
        texture.Get(),
        &shaderResourceViewDesc,
        shaderResourceView.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    SetDebugName(texture.Get(), std::string(debugNamePrefix));
    SetDebugName(shaderResourceView.Get(), std::string(debugNamePrefix) + ".SRV");
    return S_OK;
}

HRESULT Renderer::CreateTextureRenderTargetView(
    ID3D11Texture2D* texture,
    ComPtr<ID3D11RenderTargetView>& renderTargetView,
    const char* debugNamePrefix)
{
    if (texture == nullptr)
    {
        return E_POINTER;
    }

    HRESULT hr = m_device->CreateRenderTargetView(
        texture,
        nullptr,
        renderTargetView.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    SetDebugName(renderTargetView.Get(), debugNamePrefix);
    return S_OK;
}

HRESULT Renderer::RenderFullscreenTexture(
    ID3D11Texture2D* targetTexture,
    UINT width,
    UINT height,
    ID3D11PixelShader* pixelShader,
    const wchar_t* eventName)
{
    if (targetTexture == nullptr || pixelShader == nullptr)
    {
        return E_POINTER;
    }

    ComPtr<ID3D11RenderTargetView> renderTargetView;
    HRESULT hr = m_device->CreateRenderTargetView(
        targetTexture,
        nullptr,
        renderTargetView.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    BeginEvent(eventName);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<FLOAT>(width);
    viewport.Height = static_cast<FLOAT>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_deviceContext->RSSetViewports(1, &viewport);

    m_deviceContext->IASetInputLayout(nullptr);
    m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_deviceContext->RSSetState(m_rasterizerState.Get());
    m_deviceContext->OMSetDepthStencilState(m_skyDepthStencilState.Get(), 0);
    m_deviceContext->VSSetShader(m_fullscreenVertexShader.Get(), nullptr, 0);
    m_deviceContext->PSSetShader(pixelShader, nullptr, 0);

    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ID3D11RenderTargetView* renderTargets[] = { renderTargetView.Get() };
    m_deviceContext->OMSetRenderTargets(1, renderTargets, nullptr);
    m_deviceContext->ClearRenderTargetView(renderTargetView.Get(), clearColor);
    m_deviceContext->Draw(3, 0);
    m_deviceContext->OMSetRenderTargets(0, nullptr, nullptr);

    EndEvent();
    return S_OK;
}

HRESULT Renderer::CreateBrdfIntegrationMap()
{
    HRESULT hr = CreateFloatTexture2D(
        kBrdfIntegrationMapSize,
        kBrdfIntegrationMapSize,
        m_brdfIntegrationTexture,
        m_brdfIntegrationShaderResourceView,
        "IBLSpecularBrdfIntegrationMap");
    if (FAILED(hr))
    {
        return hr;
    }

    return RenderFullscreenTexture(
        m_brdfIntegrationTexture.Get(),
        kBrdfIntegrationMapSize,
        kBrdfIntegrationMapSize,
        m_brdfIntegrationPixelShader.Get(),
        L"BrdfIntegrationPass");
}

void Renderer::ReleaseModelResources()
{
    m_modelDrawItems.clear();
    m_modelPrimitives.clear();
    m_modelMaterials.clear();
}

void Renderer::InitializeLights()
{
    const XMVECTOR boundsMin = XMLoadFloat3(&m_sceneBoundsMin);
    const XMVECTOR boundsMax = XMLoadFloat3(&m_sceneBoundsMax);
    const XMVECTOR center = XMVectorScale(XMVectorAdd(boundsMin, boundsMax), 0.5f);
    const XMVECTOR extents = XMVectorScale(XMVectorSubtract(boundsMax, boundsMin), 0.5f);
    const float radius = std::max(1.0f, XMVectorGetX(XMVector3Length(extents)));

    XMFLOAT3 centerPoint = {};
    XMStoreFloat3(&centerPoint, center);

    const bool isMetalSphereDemo = (_wcsicmp(m_loadedSceneFileName.c_str(), L"MetalSphere.gltf") == 0);
    if (isMetalSphereDemo)
    {
        const float demoLightRadius = std::max(radius * 2.4f, 4.0f);
        m_pointLights[0] =
        {
            XMFLOAT3(centerPoint.x - radius * 1.25f, centerPoint.y + radius * 0.35f, centerPoint.z - radius * 1.15f),
            demoLightRadius,
            XMFLOAT3(1.0f, 0.35f, 0.25f),
            10.0f
        };
        m_pointLights[1] =
        {
            XMFLOAT3(centerPoint.x, centerPoint.y + radius * 1.1f, centerPoint.z - radius * 1.0f),
            demoLightRadius,
            XMFLOAT3(1.0f, 0.98f, 0.95f),
            12.0f
        };
        m_pointLights[2] =
        {
            XMFLOAT3(centerPoint.x + radius * 1.25f, centerPoint.y + radius * 0.35f, centerPoint.z - radius * 1.15f),
            demoLightRadius,
            XMFLOAT3(0.25f, 0.55f, 1.0f),
            10.0f
        };
        return;
    }

    const float lightRadius = std::max(radius * 4.5f, 8.0f);
    m_pointLights[0] =
    {
        XMFLOAT3(centerPoint.x - radius * 1.4f, centerPoint.y + radius * 0.9f, centerPoint.z - radius * 1.45f),
        lightRadius,
        XMFLOAT3(1.0f, 0.28f, 0.22f),
        2.4f
    };
    m_pointLights[1] =
    {
        XMFLOAT3(centerPoint.x, centerPoint.y + radius * 1.55f, centerPoint.z - radius * 1.8f),
        lightRadius,
        XMFLOAT3(0.95f, 0.97f, 1.0f),
        2.7f
    };
    m_pointLights[2] =
    {
        XMFLOAT3(centerPoint.x + radius * 1.4f, centerPoint.y + radius * 0.9f, centerPoint.z - radius * 1.45f),
        lightRadius,
        XMFLOAT3(0.22f, 0.52f, 1.0f),
        2.4f
    };
}

void Renderer::SetInitializationError(std::wstring message)
{
    m_initializationErrorMessage = std::move(message);
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
    case DisplayMode::DirectLighting:
        modeName = L"Direct";
        break;
    case DisplayMode::Pbr:
    default:
        modeName = L"PBR";
        break;
    }

    const std::wstring title =
        m_title +
        L" | B: Bloom " +
        std::wstring(m_bloomEnabled ? L"On" : L"Off") +
        L" | L: Lights " +
        std::wstring(m_pointLightsEnabled ? L"On" : L"Off") +
        L" | R: Reset | 1-5: View | Mode: " +
        std::wstring(modeName) +
        L" | Model: " +
        m_loadedSceneFileName;

    SetWindowTextW(m_hwnd, title.c_str());
}

void Renderer::ResetCamera()
{
    const XMVECTOR boundsMin = XMLoadFloat3(&m_sceneBoundsMin);
    const XMVECTOR boundsMax = XMLoadFloat3(&m_sceneBoundsMax);
    const XMVECTOR center = XMVectorScale(XMVectorAdd(boundsMin, boundsMax), 0.5f);
    const XMVECTOR extents = XMVectorScale(XMVectorSubtract(boundsMax, boundsMin), 0.5f);
    const float radius = std::max(1.5f, XMVectorGetX(XMVector3Length(extents)));

    XMStoreFloat3(&m_cameraTarget, center);
    m_cameraDistance = std::clamp(radius * 3.1f, 4.0f, 55.0f);
    m_cameraYaw = 0.72f;
    m_cameraPitch = -0.26f;
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
    const XMMATRIX projectionMatrix = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), aspectRatio, 0.1f, 300.0f);

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
    ID3D11Buffer* vertexBuffers[] = { m_environmentSphereGeometry.vertexBuffer.Get() };
    m_deviceContext->IASetInputLayout(m_sceneInputLayout.Get());
    m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_deviceContext->IASetVertexBuffers(0, 1, vertexBuffers, &stride, &offset);
    m_deviceContext->IASetIndexBuffer(m_environmentSphereGeometry.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);

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
    m_deviceContext->DrawIndexed(m_environmentSphereGeometry.indexCount, 0, 0);

    ID3D11ShaderResourceView* nullSrvs[] = { nullptr };
    m_deviceContext->PSSetShaderResources(0, 1, nullSrvs);

    EndEvent();
}

void Renderer::RenderModel()
{
    if (m_modelDrawItems.empty())
    {
        return;
    }

    BeginEvent(L"GLTFScenePass");

    SceneFrameConstants sceneFrameConstants = {};
    sceneFrameConstants.viewProjection = StoreMatrix(XMLoadFloat4x4(&m_viewMatrix) * XMLoadFloat4x4(&m_projectionMatrix));
    sceneFrameConstants.cameraPosition = XMFLOAT4(m_cameraPosition.x, m_cameraPosition.y, m_cameraPosition.z, 1.0f);

    for (size_t lightIndex = 0; lightIndex < m_pointLights.size(); ++lightIndex)
    {
        const PointLight& light = m_pointLights[lightIndex];
        sceneFrameConstants.pointLights[lightIndex].positionRadius =
            XMFLOAT4(light.position.x, light.position.y, light.position.z, light.radius);
        sceneFrameConstants.pointLights[lightIndex].colorIntensity =
            XMFLOAT4(light.color.x, light.color.y, light.color.z, m_pointLightsEnabled ? light.intensity : 0.0f);
    }

    sceneFrameConstants.globalParameters =
        XMFLOAT4(
            static_cast<float>(static_cast<int>(m_displayMode)),
            kEnvironmentIntensity,
            kMaxReflectionLod,
            m_bloomThreshold);
    UpdateConstantBuffer(m_deviceContext.Get(), m_sceneFrameConstantBuffer, sceneFrameConstants);

    m_deviceContext->IASetInputLayout(m_sceneInputLayout.Get());
    m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_deviceContext->VSSetShader(m_pbrSceneVertexShader.Get(), nullptr, 0);
    m_deviceContext->PSSetShader(m_pbrScenePixelShader.Get(), nullptr, 0);

    ID3D11Buffer* sceneFrameBuffers[] = { m_sceneFrameConstantBuffer.Get() };
    m_deviceContext->VSSetConstantBuffers(0, 1, sceneFrameBuffers);
    m_deviceContext->PSSetConstantBuffers(0, 1, sceneFrameBuffers);

    ID3D11SamplerState* samplers[] = { m_linearClampSampler.Get(), m_linearWrapSampler.Get() };
    m_deviceContext->PSSetSamplers(0, ARRAYSIZE(samplers), samplers);

    ID3D11ShaderResourceView* shaderResources[] =
    {
        m_irradianceCubemapShaderResourceView.Get(),
        m_prefilteredEnvironmentCubemapShaderResourceView.Get(),
        m_brdfIntegrationShaderResourceView.Get(),
    };
    m_deviceContext->PSSetShaderResources(0, ARRAYSIZE(shaderResources), shaderResources);

    for (const ModelDrawItem& drawItem : m_modelDrawItems)
    {
        const ModelPrimitive& primitive = m_modelPrimitives[drawItem.primitiveIndex];
        const ModelMaterial& material =
            (primitive.materialIndex < m_modelMaterials.size())
            ? m_modelMaterials[primitive.materialIndex]
            : m_modelMaterials.front();

        if (material.alphaMode == Gltf::AlphaMode::Blend)
        {
            continue;
        }

        const float blendFactors[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        m_deviceContext->OMSetDepthStencilState(m_depthStencilState.Get(), 0);
        m_deviceContext->OMSetBlendState(nullptr, blendFactors, 0xFFFFFFFFu);
        DrawModelDrawItem(drawItem);
    }

    const float blendFactors[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_deviceContext->OMSetDepthStencilState(m_transparentDepthStencilState.Get(), 0);
    m_deviceContext->OMSetBlendState(m_alphaBlendState.Get(), blendFactors, 0xFFFFFFFFu);
    for (const ModelDrawItem& drawItem : m_modelDrawItems)
    {
        const ModelPrimitive& primitive = m_modelPrimitives[drawItem.primitiveIndex];
        const ModelMaterial& material =
            (primitive.materialIndex < m_modelMaterials.size())
            ? m_modelMaterials[primitive.materialIndex]
            : m_modelMaterials.front();

        if (material.alphaMode != Gltf::AlphaMode::Blend)
        {
            continue;
        }

        DrawModelDrawItem(drawItem);
    }

    for (size_t lightIndex = 0; lightIndex < m_pointLights.size(); ++lightIndex)
    {
        if (m_displayMode != DisplayMode::Pbr || !m_pointLightsEnabled)
        {
            continue;
        }

        const PointLight& light = m_pointLights[lightIndex];
        ModelMaterial markerMaterial = {};
        markerMaterial.baseColorFactor = XMFLOAT4(light.color.x, light.color.y, light.color.z, 1.0f);
        markerMaterial.emissiveFactor = XMFLOAT4(light.color.x, light.color.y, light.color.z, 1.0f);
        markerMaterial.roughnessFactor = 0.05f;
        markerMaterial.metallicFactor = 1.0f;
        markerMaterial.occlusionStrength = 1.0f;
        markerMaterial.alphaCutoff = 0.5f;
        markerMaterial.doubleSided = false;
        markerMaterial.alphaMode = Gltf::AlphaMode::Opaque;
        markerMaterial.baseColorTextureShaderResourceView = m_defaultWhiteSrgbShaderResourceView;
        markerMaterial.metallicRoughnessTextureShaderResourceView = m_defaultWhiteLinearShaderResourceView;
        markerMaterial.emissiveTextureShaderResourceView = m_defaultWhiteSrgbShaderResourceView;
        markerMaterial.occlusionTextureShaderResourceView = m_defaultWhiteLinearShaderResourceView;
        m_deviceContext->OMSetDepthStencilState(m_depthStencilState.Get(), 0);
        m_deviceContext->OMSetBlendState(nullptr, blendFactors, 0xFFFFFFFFu);
        DrawMesh(
            m_sphereGeometry,
            CreateWorldMatrix(XMFLOAT3(kLightMarkerScale, kLightMarkerScale, kLightMarkerScale), light.position),
            markerMaterial,
            6.0f);
    }

    ID3D11ShaderResourceView* nullSrvs[] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    m_deviceContext->PSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
    m_deviceContext->OMSetBlendState(nullptr, blendFactors, 0xFFFFFFFFu);

    EndEvent();
}

void Renderer::ApplyBloom()
{
    if (!m_bloomEnabled ||
        m_bloomBlurPixelShader == nullptr ||
        m_fullscreenVertexShader == nullptr ||
        m_postProcessConstantBuffer == nullptr ||
        m_bloomSourceShaderResourceView == nullptr ||
        m_bloomTextureAShaderResourceView == nullptr ||
        m_bloomTextureARenderTargetView == nullptr ||
        m_bloomTextureBShaderResourceView == nullptr ||
        m_bloomTextureBRenderTargetView == nullptr)
    {
        return;
    }

    BeginEvent(L"BloomPass");

    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const XMFLOAT2 inverseTextureSize(
        (m_width > 0) ? (1.0f / static_cast<float>(m_width)) : 0.0f,
        (m_height > 0) ? (1.0f / static_cast<float>(m_height)) : 0.0f);

    ID3D11Buffer* postProcessBuffers[] = { m_postProcessConstantBuffer.Get() };
    ID3D11SamplerState* samplers[] = { m_linearClampSampler.Get() };

    m_deviceContext->IASetInputLayout(nullptr);
    m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_deviceContext->RSSetState(m_rasterizerState.Get());
    m_deviceContext->OMSetDepthStencilState(m_skyDepthStencilState.Get(), 0);
    m_deviceContext->OMSetBlendState(nullptr, clearColor, 0xFFFFFFFFu);
    m_deviceContext->VSSetShader(m_fullscreenVertexShader.Get(), nullptr, 0);
    m_deviceContext->PSSetShader(m_bloomBlurPixelShader.Get(), nullptr, 0);
    m_deviceContext->PSSetConstantBuffers(0, 1, postProcessBuffers);
    m_deviceContext->PSSetSamplers(0, 1, samplers);
    SetViewport(m_width, m_height);

    auto renderBlurPass = [this, &clearColor, &inverseTextureSize](
        ID3D11ShaderResourceView* sourceShaderResourceView,
        ID3D11RenderTargetView* destinationRenderTargetView,
        const XMFLOAT2& blurDirection)
    {
        PostProcessConstants postProcessConstants = {};
        postProcessConstants.inverseTextureSize = inverseTextureSize;
        postProcessConstants.blurDirection = blurDirection;
        postProcessConstants.parameters = XMFLOAT4(2.0f, 0.0f, 0.0f, 0.0f);
        UpdateConstantBuffer(m_deviceContext.Get(), m_postProcessConstantBuffer, postProcessConstants);

        ID3D11RenderTargetView* renderTargets[] = { destinationRenderTargetView };
        m_deviceContext->OMSetRenderTargets(1, renderTargets, nullptr);
        m_deviceContext->ClearRenderTargetView(destinationRenderTargetView, clearColor);

        ID3D11ShaderResourceView* shaderResources[] = { sourceShaderResourceView };
        m_deviceContext->PSSetShaderResources(0, 1, shaderResources);
        m_deviceContext->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrvs[] = { nullptr };
        m_deviceContext->PSSetShaderResources(0, 1, nullSrvs);
    };

    ID3D11ShaderResourceView* currentSourceShaderResourceView = m_bloomSourceShaderResourceView.Get();
    for (UINT blurPair = 0; blurPair < kBloomBlurPairCount; ++blurPair)
    {
        renderBlurPass(
            currentSourceShaderResourceView,
            m_bloomTextureBRenderTargetView.Get(),
            XMFLOAT2(1.0f, 0.0f));
        renderBlurPass(
            m_bloomTextureBShaderResourceView.Get(),
            m_bloomTextureARenderTargetView.Get(),
            XMFLOAT2(0.0f, 1.0f));
        currentSourceShaderResourceView = m_bloomTextureAShaderResourceView.Get();
    }

    m_deviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    EndEvent();
}

void Renderer::CompositeScene()
{
    if (m_renderTargetView == nullptr ||
        m_hdrSceneShaderResourceView == nullptr ||
        m_bloomSourceShaderResourceView == nullptr ||
        m_bloomTextureAShaderResourceView == nullptr ||
        m_bloomCompositePixelShader == nullptr ||
        m_fullscreenVertexShader == nullptr ||
        m_postProcessConstantBuffer == nullptr)
    {
        return;
    }

    BeginEvent(L"CompositePass");

    PostProcessConstants postProcessConstants = {};
    postProcessConstants.inverseTextureSize = XMFLOAT2(
        (m_width > 0) ? (1.0f / static_cast<float>(m_width)) : 0.0f,
        (m_height > 0) ? (1.0f / static_cast<float>(m_height)) : 0.0f);
    postProcessConstants.parameters = XMFLOAT4(
        m_bloomEnabled ? m_bloomIntensity : 0.0f,
        1.2f,
        0.0f,
        0.0f);
    UpdateConstantBuffer(m_deviceContext.Get(), m_postProcessConstantBuffer, postProcessConstants);

    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ID3D11RenderTargetView* renderTargets[] = { m_renderTargetView.Get() };
    ID3D11Buffer* postProcessBuffers[] = { m_postProcessConstantBuffer.Get() };
    ID3D11SamplerState* samplers[] = { m_linearClampSampler.Get() };
    ID3D11ShaderResourceView* shaderResources[] =
    {
        m_hdrSceneShaderResourceView.Get(),
        m_bloomTextureAShaderResourceView.Get(),
        m_bloomSourceShaderResourceView.Get(),
    };

    m_deviceContext->IASetInputLayout(nullptr);
    m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_deviceContext->RSSetState(m_rasterizerState.Get());
    m_deviceContext->OMSetDepthStencilState(m_skyDepthStencilState.Get(), 0);
    m_deviceContext->OMSetBlendState(nullptr, clearColor, 0xFFFFFFFFu);
    m_deviceContext->OMSetRenderTargets(1, renderTargets, nullptr);
    m_deviceContext->VSSetShader(m_fullscreenVertexShader.Get(), nullptr, 0);
    m_deviceContext->PSSetShader(m_bloomCompositePixelShader.Get(), nullptr, 0);
    m_deviceContext->PSSetConstantBuffers(0, 1, postProcessBuffers);
    m_deviceContext->PSSetSamplers(0, 1, samplers);
    m_deviceContext->PSSetShaderResources(0, ARRAYSIZE(shaderResources), shaderResources);
    m_deviceContext->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);
    SetViewport(m_width, m_height);
    m_deviceContext->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrvs[] = { nullptr, nullptr, nullptr };
    m_deviceContext->PSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
    m_deviceContext->OMSetRenderTargets(0, nullptr, nullptr);

    EndEvent();
}

void Renderer::DrawModelDrawItem(const ModelDrawItem& object)
{
    if (object.primitiveIndex >= m_modelPrimitives.size())
    {
        return;
    }

    const ModelPrimitive& primitive = m_modelPrimitives[object.primitiveIndex];
    const ModelMaterial& material =
        (primitive.materialIndex < m_modelMaterials.size())
        ? m_modelMaterials[primitive.materialIndex]
        : m_modelMaterials.front();
    DrawMesh(primitive.geometry, object.world, material);
}

void Renderer::DrawMesh(
    const MeshGeometry& geometry,
    const XMFLOAT4X4& world,
    const ModelMaterial& material,
    float emissiveMultiplier)
{
    const UINT stride = sizeof(SceneVertex);
    const UINT offset = 0;
    ID3D11Buffer* vertexBuffers[] = { geometry.vertexBuffer.Get() };
    m_deviceContext->IASetVertexBuffers(0, 1, vertexBuffers, &stride, &offset);
    m_deviceContext->IASetIndexBuffer(geometry.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
    m_deviceContext->RSSetState(material.doubleSided ? m_doubleSidedRasterizerState.Get() : m_rasterizerState.Get());

    const XMMATRIX worldMatrix = XMLoadFloat4x4(&world);
    const XMMATRIX normalMatrix = XMMatrixInverse(nullptr, worldMatrix);

    SceneObjectConstants objectConstants = {};
    objectConstants.world = StoreMatrix(worldMatrix);
    objectConstants.normalMatrix = StoreMatrix(normalMatrix);
    objectConstants.baseColorFactor = material.baseColorFactor;
    objectConstants.emissiveFactor = XMFLOAT4(
        material.emissiveFactor.x * emissiveMultiplier,
        material.emissiveFactor.y * emissiveMultiplier,
        material.emissiveFactor.z * emissiveMultiplier,
        material.emissiveFactor.w);
    objectConstants.materialParameters = XMFLOAT4(
        material.roughnessFactor,
        material.metallicFactor,
        material.occlusionStrength,
        material.alphaCutoff);
    objectConstants.materialFlags = XMFLOAT4(static_cast<float>(static_cast<int>(material.alphaMode)), 0.0f, 0.0f, 0.0f);
    UpdateConstantBuffer(m_deviceContext.Get(), m_sceneObjectConstantBuffer, objectConstants);

    ID3D11Buffer* objectBuffers[] = { m_sceneObjectConstantBuffer.Get() };
    m_deviceContext->VSSetConstantBuffers(1, 1, objectBuffers);
    m_deviceContext->PSSetConstantBuffers(1, 1, objectBuffers);

    ID3D11ShaderResourceView* materialShaderResources[] =
    {
        material.baseColorTextureShaderResourceView.Get(),
        material.metallicRoughnessTextureShaderResourceView.Get(),
        material.emissiveTextureShaderResourceView.Get(),
        material.occlusionTextureShaderResourceView.Get(),
    };
    m_deviceContext->PSSetShaderResources(3, ARRAYSIZE(materialShaderResources), materialShaderResources);

    m_deviceContext->DrawIndexed(geometry.indexCount, 0, 0);
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
        sourceShaderCandidates.emplace_back(currentDirectory / L"ibl-diffuse" / sourceRelativePath);
    }

    sourceShaderCandidates.emplace_back(executableDirectory / sourceRelativePath);

    const fs::path twoLevelsUp = executableDirectory.parent_path().parent_path();
    if (!twoLevelsUp.empty())
    {
        sourceShaderCandidates.emplace_back(twoLevelsUp / sourceRelativePath);
        sourceShaderCandidates.emplace_back(twoLevelsUp / L"ibl-diffuse" / sourceRelativePath);
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

fs::path Renderer::FindHdriFile() const
{
    const fs::path executableDirectory = GetExecutableDirectory();

    std::error_code errorCode;
    const fs::path currentDirectory = fs::current_path(errorCode);

    std::vector<fs::path> searchDirectories;
    searchDirectories.emplace_back(executableDirectory / L"assets" / L"hdri");

    if (!currentDirectory.empty())
    {
        searchDirectories.emplace_back(currentDirectory / L"assets" / L"hdri");
        searchDirectories.emplace_back(currentDirectory / L"ibl-diffuse" / L"assets" / L"hdri");
    }

    const fs::path twoLevelsUp = executableDirectory.parent_path().parent_path();
    if (!twoLevelsUp.empty())
    {
        searchDirectories.emplace_back(twoLevelsUp / L"assets" / L"hdri");
        searchDirectories.emplace_back(twoLevelsUp / L"ibl-diffuse" / L"assets" / L"hdri");
    }

    std::vector<fs::path> candidateFiles;
    for (const fs::path& directory : searchDirectories)
    {
        AppendHdrFilesFromDirectory(directory, candidateFiles);
    }

    if (candidateFiles.empty())
    {
        return {};
    }

    std::sort(candidateFiles.begin(), candidateFiles.end());
    candidateFiles.erase(std::unique(candidateFiles.begin(), candidateFiles.end()), candidateFiles.end());
    return candidateFiles.front();
}

fs::path Renderer::FindSceneFile() const
{
    const fs::path executableDirectory = GetExecutableDirectory();

    std::error_code errorCode;
    const fs::path currentDirectory = fs::current_path(errorCode);

    std::vector<fs::path> searchDirectories;
    searchDirectories.emplace_back(executableDirectory / L"assets" / L"models");

    if (!currentDirectory.empty())
    {
        searchDirectories.emplace_back(currentDirectory / L"assets" / L"models");
        searchDirectories.emplace_back(currentDirectory / L"ibl-diffuse" / L"assets" / L"models");
    }

    const fs::path twoLevelsUp = executableDirectory.parent_path().parent_path();
    if (!twoLevelsUp.empty())
    {
        searchDirectories.emplace_back(twoLevelsUp / L"assets" / L"models");
        searchDirectories.emplace_back(twoLevelsUp / L"ibl-diffuse" / L"assets" / L"models");
    }

    std::vector<fs::path> candidateFiles;
    for (const fs::path& directory : searchDirectories)
    {
        AppendSceneFilesFromDirectory(directory, candidateFiles);
    }

    if (candidateFiles.empty())
    {
        return {};
    }

    std::sort(candidateFiles.begin(), candidateFiles.end());
    candidateFiles.erase(std::unique(candidateFiles.begin(), candidateFiles.end()), candidateFiles.end());
    return candidateFiles.front();
}

void Renderer::Render()
{
    if (m_isMinimized || m_width == 0 || m_height == 0)
    {
        Sleep(16);
        return;
    }

    BeginEvent(L"Frame");

    ID3D11RenderTargetView* renderTargets[] =
    {
        m_hdrSceneRenderTargetView.Get(),
        m_bloomSourceRenderTargetView.Get(),
    };
    m_deviceContext->OMSetRenderTargets(ARRAYSIZE(renderTargets), renderTargets, m_depthStencilView.Get());
    SetViewport(m_width, m_height);

    const float clearColor[4] = { 0.02f, 0.02f, 0.025f, 1.0f };
    const float bloomClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_deviceContext->ClearRenderTargetView(m_hdrSceneRenderTargetView.Get(), clearColor);
    m_deviceContext->ClearRenderTargetView(m_bloomSourceRenderTargetView.Get(), bloomClearColor);
    m_deviceContext->ClearDepthStencilView(m_depthStencilView.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    RenderEnvironment();
    RenderModel();
    ApplyBloom();
    CompositeScene();

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

    ReleaseModelResources();
    m_sphereGeometry = {};
    m_environmentSphereGeometry = {};
    m_captureConstantBuffer.Reset();
    m_postProcessConstantBuffer.Reset();
    m_sceneFrameConstantBuffer.Reset();
    m_sceneObjectConstantBuffer.Reset();
    m_skyFrameConstantBuffer.Reset();
    m_sceneInputLayout.Reset();
    m_captureVertexShader.Reset();
    m_fullscreenVertexShader.Reset();
    m_bloomBlurPixelShader.Reset();
    m_bloomCompositePixelShader.Reset();
    m_brdfIntegrationPixelShader.Reset();
    m_equirectangularToCubemapPixelShader.Reset();
    m_irradianceConvolutionPixelShader.Reset();
    m_prefilterEnvironmentPixelShader.Reset();
    m_pbrSceneVertexShader.Reset();
    m_pbrScenePixelShader.Reset();
    m_skyVertexShader.Reset();
    m_skyPixelShader.Reset();
    m_linearClampSampler.Reset();
    m_linearWrapSampler.Reset();
    m_brdfIntegrationShaderResourceView.Reset();
    m_brdfIntegrationTexture.Reset();
    m_prefilteredEnvironmentCubemapShaderResourceView.Reset();
    m_prefilteredEnvironmentCubemap.Reset();
    m_irradianceCubemapShaderResourceView.Reset();
    m_irradianceCubemap.Reset();
    m_hdriTextureShaderResourceView.Reset();
    m_hdriTexture.Reset();
    m_environmentCubemapShaderResourceView.Reset();
    m_environmentCubemap.Reset();
    m_defaultWhiteLinearShaderResourceView.Reset();
    m_defaultWhiteSrgbShaderResourceView.Reset();
    m_skyDepthStencilState.Reset();
    m_transparentDepthStencilState.Reset();
    m_depthStencilState.Reset();
    m_alphaBlendState.Reset();
    m_skyRasterizerState.Reset();
    m_doubleSidedRasterizerState.Reset();
    m_rasterizerState.Reset();
    m_annotation.Reset();
    m_swapChain.Reset();
    m_deviceContext.Reset();
    m_device.Reset();

    if (m_comInitialized)
    {
        CoUninitialize();
        m_comInitialized = false;
    }
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
        else if (wParam == '5')
        {
            m_displayMode = DisplayMode::DirectLighting;
            UpdateWindowTitle();
        }
        else if (wParam == 'R')
        {
            ResetCamera();
        }
        else if (wParam == 'L')
        {
            m_pointLightsEnabled = !m_pointLightsEnabled;
            UpdateWindowTitle();
        }
        else if (wParam == 'B')
        {
            m_bloomEnabled = !m_bloomEnabled;
            UpdateWindowTitle();
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
