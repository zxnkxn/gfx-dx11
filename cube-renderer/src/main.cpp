#include <windows.h>
#include <iostream>
#include <fstream>
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
    // Check if shader files exist
    std::ifstream vsFile("src/shaders/vertex_shader.hlsl");
    std::ifstream psFile("src/shaders/pixel_shader.hlsl");

    if (!vsFile.is_open()) {
        MessageBox(nullptr, L"Vertex shader file not found!", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    vsFile.close();

    if (!psFile.is_open()) {
        MessageBox(nullptr, L"Pixel shader file not found!", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    psFile.close();

    // Initialize the renderer
    HRESULT hr = g_renderer.Initialize(hInstance, nCmdShow);
    if (FAILED(hr))
    {
        MessageBox(nullptr, L"Failed to initialize DirectX renderer", L"Error", MB_OK | MB_ICONERROR);
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