#include <cwchar>
#include <string>
#include <windows.h>

#include "renderer.h"

// Global renderer instance
Renderer g_renderer;

// Main entry point
int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    const HRESULT hr = g_renderer.Initialize(hInstance, nCmdShow);
    if (FAILED(hr))
    {
        std::wstring message =
            L"Failed to initialize the glTF metallic-roughness viewer.\n";

        const std::wstring& details = g_renderer.GetInitializationErrorMessage();
        if (!details.empty())
        {
            message += L"\n";
            message += details;
            message += L"\n";
        }
        else
        {
            message +=
                L"Make sure a Radiance HDR file (*.hdr) exists in ibl-diffuse\\assets\\hdri\n"
                L"and a glTF 2.0 model (*.gltf or *.glb) exists in ibl-diffuse\\assets\\models.\n";
        }

        wchar_t hrText[32] = {};
        swprintf_s(hrText, L"0x%08X", static_cast<unsigned int>(hr));
        message += L"HRESULT: ";
        message += hrText;

        MessageBoxW(nullptr, message.c_str(), L"Initialization Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        else
        {
            g_renderer.Render();
        }
    }

    return static_cast<int>(msg.wParam);
}
