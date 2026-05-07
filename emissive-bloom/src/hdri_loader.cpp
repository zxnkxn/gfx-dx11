#include "hdri_loader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    struct RgbePixel
    {
        std::uint8_t r = 0;
        std::uint8_t g = 0;
        std::uint8_t b = 0;
        std::uint8_t e = 0;
    };

    bool ReadExact(std::istream& stream, void* destination, std::size_t byteCount)
    {
        stream.read(static_cast<char*>(destination), static_cast<std::streamsize>(byteCount));
        return stream.good();
    }

    void ConvertRgbEToRgba32f(const RgbePixel& pixel, float* destination)
    {
        if (pixel.e == 0)
        {
            destination[0] = 0.0f;
            destination[1] = 0.0f;
            destination[2] = 0.0f;
            destination[3] = 1.0f;
            return;
        }

        const float scale = std::ldexp(1.0f, static_cast<int>(pixel.e) - (128 + 8));
        destination[0] = static_cast<float>(pixel.r) * scale;
        destination[1] = static_cast<float>(pixel.g) * scale;
        destination[2] = static_cast<float>(pixel.b) * scale;
        destination[3] = 1.0f;
    }

    HRESULT ParseResolutionLine(
        const std::string& line,
        std::uint32_t& width,
        std::uint32_t& height,
        bool& flipX,
        bool& flipY)
    {
        std::istringstream stream(line);

        char ySign = '\0';
        char yAxis = '\0';
        int parsedHeight = 0;
        char xSign = '\0';
        char xAxis = '\0';
        int parsedWidth = 0;

        if (!(stream >> ySign >> yAxis >> parsedHeight >> xSign >> xAxis >> parsedWidth))
        {
            return E_FAIL;
        }

        if (parsedWidth <= 0 || parsedHeight <= 0 || yAxis != 'Y' || xAxis != 'X')
        {
            return E_FAIL;
        }

        width = static_cast<std::uint32_t>(parsedWidth);
        height = static_cast<std::uint32_t>(parsedHeight);
        flipX = (xSign == '-');
        // Radiance uses "-Y +X" for top-to-bottom scanlines.
        flipY = (ySign == '+');
        return S_OK;
    }

    HRESULT ReadFlatScanline(
        std::istream& stream,
        std::uint32_t width,
        const RgbePixel& firstPixel,
        std::vector<RgbePixel>& scanline)
    {
        scanline.resize(width);
        scanline[0] = firstPixel;

        std::uint32_t pixelIndex = 1;
        while (pixelIndex < width)
        {
            RgbePixel pixel = {};
            if (!ReadExact(stream, &pixel, sizeof(pixel)))
            {
                return E_FAIL;
            }

            // Support the legacy repeat encoding used by some old HDR files.
            if (pixel.r == 1 && pixel.g == 1 && pixel.b == 1)
            {
                const std::uint32_t repeatCount = pixel.e;
                if (repeatCount == 0 || pixelIndex == 0)
                {
                    return E_FAIL;
                }

                const RgbePixel previousPixel = scanline[pixelIndex - 1];
                for (std::uint32_t repeatIndex = 0; repeatIndex < repeatCount && pixelIndex < width; ++repeatIndex)
                {
                    scanline[pixelIndex++] = previousPixel;
                }

                continue;
            }

            scanline[pixelIndex++] = pixel;
        }

        return S_OK;
    }

    HRESULT ReadRleScanline(std::istream& stream, std::uint32_t width, std::vector<RgbePixel>& scanline)
    {
        std::array<std::uint8_t, 4> header = {};
        if (!ReadExact(stream, header.data(), header.size()))
        {
            return E_FAIL;
        }

        const bool usesModernRle =
            width >= 8 &&
            width <= 0x7FFF &&
            header[0] == 2 &&
            header[1] == 2 &&
            (header[2] & 0x80u) == 0 &&
            (((static_cast<std::uint32_t>(header[2]) << 8u) | header[3]) == width);

        if (!usesModernRle)
        {
            const RgbePixel firstPixel = { header[0], header[1], header[2], header[3] };
            return ReadFlatScanline(stream, width, firstPixel, scanline);
        }

        scanline.resize(width);
        std::array<std::vector<std::uint8_t>, 4> channels;
        for (std::vector<std::uint8_t>& channel : channels)
        {
            channel.resize(width);
        }

        for (std::uint32_t channelIndex = 0; channelIndex < 4; ++channelIndex)
        {
            std::uint32_t valueIndex = 0;
            while (valueIndex < width)
            {
                std::array<std::uint8_t, 2> packet = {};
                if (!ReadExact(stream, packet.data(), packet.size()))
                {
                    return E_FAIL;
                }

                if (packet[0] > 128u)
                {
                    const std::uint32_t runLength = packet[0] - 128u;
                    if (runLength == 0 || valueIndex + runLength > width)
                    {
                        return E_FAIL;
                    }

                    std::fill_n(channels[channelIndex].begin() + valueIndex, runLength, packet[1]);
                    valueIndex += runLength;
                }
                else
                {
                    const std::uint32_t literalCount = packet[0];
                    if (literalCount == 0 || valueIndex + literalCount > width)
                    {
                        return E_FAIL;
                    }

                    channels[channelIndex][valueIndex++] = packet[1];
                    for (std::uint32_t literalIndex = 1; literalIndex < literalCount; ++literalIndex)
                    {
                        if (!ReadExact(stream, &channels[channelIndex][valueIndex], sizeof(std::uint8_t)))
                        {
                            return E_FAIL;
                        }

                        ++valueIndex;
                    }
                }
            }
        }

        for (std::uint32_t x = 0; x < width; ++x)
        {
            scanline[x] = { channels[0][x], channels[1][x], channels[2][x], channels[3][x] };
        }

        return S_OK;
    }
}

HRESULT LoadRadianceHdrImage(const std::filesystem::path& filePath, HdriImage& image)
{
    image = {};

    std::ifstream stream(filePath, std::ios::binary);
    if (!stream.is_open())
    {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    std::string line;
    bool foundFormat = false;
    bool foundResolution = false;
    bool flipX = false;
    bool flipY = false;

    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        if (line.empty())
        {
            continue;
        }

        if (line.rfind("FORMAT=", 0) == 0)
        {
            if (line != "FORMAT=32-bit_rle_rgbe")
            {
                return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            }

            foundFormat = true;
            continue;
        }

        if (line[0] == '-' || line[0] == '+')
        {
            const HRESULT hr = ParseResolutionLine(line, image.width, image.height, flipX, flipY);
            if (FAILED(hr))
            {
                return hr;
            }

            foundResolution = true;
            break;
        }
    }

    if (!foundFormat || !foundResolution || image.width == 0 || image.height == 0)
    {
        return E_FAIL;
    }

    image.pixels.resize(static_cast<std::size_t>(image.width) * image.height * 4u);

    std::vector<RgbePixel> scanline;
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        const HRESULT hr = ReadRleScanline(stream, image.width, scanline);
        if (FAILED(hr))
        {
            return hr;
        }

        const std::uint32_t destinationY = flipY ? (image.height - 1u - y) : y;
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const std::uint32_t destinationX = flipX ? (image.width - 1u - x) : x;
            float* destinationPixel =
                image.pixels.data() + (static_cast<std::size_t>(destinationY) * image.width + destinationX) * 4u;
            ConvertRgbEToRgba32f(scanline[x], destinationPixel);
        }
    }

    return S_OK;
}
