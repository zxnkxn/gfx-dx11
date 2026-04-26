#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <vector>

struct HdriImage
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> pixels;
};

HRESULT LoadRadianceHdrImage(const std::filesystem::path& filePath, HdriImage& image);
