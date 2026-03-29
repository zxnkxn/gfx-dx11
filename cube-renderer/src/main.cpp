#include <windows.h>
#include <cwchar>

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

    // Initialize the renderer
    const HRESULT hr = g_renderer.Initialize(hInstance, nCmdShow);
    if (FAILED(hr))
    {
        wchar_t message[256] = {};
        swprintf_s(
            message,
            L"Failed to initialize DirectX renderer.\nHRESULT: 0x%08X",
            static_cast<unsigned int>(hr));

        MessageBox(nullptr, message, L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Main message loop
    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        // Handle Windows messages
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            // Render a frame
            g_renderer.Render();
        }
    }

    return static_cast<int>(msg.wParam);
}
