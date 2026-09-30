#include "Asset/TgaLoader.h"

#include <cstddef>
#include <fstream>

namespace
{
// TGA 头部固定 18 字节。字段位置见 TgaLoader.h 的说明。
constexpr std::size_t kTgaHeaderSize = 18;
constexpr std::uint8_t kImageTypeUncompressedTrueColor = 2;
} // namespace

bool LoadTgaFromFile(const std::string& path, ImageData& outImage, std::string& outError)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        outError = "TGA: cannot open file: " + path;
        return false;
    }

    std::uint8_t header[kTgaHeaderSize] = {};
    file.read(reinterpret_cast<char*>(header), static_cast<std::streamsize>(kTgaHeaderSize));
    if (file.gcount() != static_cast<std::streamsize>(kTgaHeaderSize))
    {
        outError = "TGA: file is smaller than the 18-byte header: " + path;
        return false;
    }

    const std::uint8_t  idLength        = header[0];
    const std::uint8_t  colorMapType    = header[1];
    const std::uint8_t  imageType       = header[2];
    const std::uint16_t width           = static_cast<std::uint16_t>(header[12] | (header[13] << 8));
    const std::uint16_t height          = static_cast<std::uint16_t>(header[14] | (header[15] << 8));
    const std::uint8_t  pixelDepth      = header[16];
    const std::uint8_t  imageDescriptor = header[17];

    if (colorMapType != 0)
    {
        outError = "TGA: color-mapped images are not supported";
        return false;
    }
    if (imageType != kImageTypeUncompressedTrueColor)
    {
        outError = "TGA: only uncompressed true-color images (imageType = 2) are supported";
        return false;
    }
    if (pixelDepth != 24 && pixelDepth != 32)
    {
        outError = "TGA: only 24-bit and 32-bit pixels are supported";
        return false;
    }
    if (width == 0 || height == 0)
    {
        outError = "TGA: zero-sized image";
        return false;
    }

    // 头部之后是可选的图像 ID 字段，长度由 idLength 指定，先跳过。
    if (idLength > 0)
    {
        file.seekg(idLength, std::ios::cur);
    }

    const std::size_t bytesPerPixel = pixelDepth / 8;
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;

    std::vector<std::uint8_t> raw(pixelCount * bytesPerPixel);
    file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    if (file.gcount() != static_cast<std::streamsize>(raw.size()))
    {
        outError = "TGA: truncated pixel data";
        return false;
    }

    outImage.width = width;
    outImage.height = height;
    outImage.pixels.resize(pixelCount * 4);

    // imageDescriptor 的 bit5：1 = 左上为原点，0 = 左下为原点。
    // 这里统一输出「左上原点、行优先」，正好与 D3D 纹理坐标（v = 0 在顶部）一致，
    // 因此后续不需要再做任何翻转。
    const bool topLeftOrigin = (imageDescriptor & 0x20) != 0;

    for (std::uint32_t y = 0; y < height; ++y)
    {
        const std::uint32_t sourceY = topLeftOrigin ? y : (height - 1 - y);
        const std::uint8_t* sourceRow =
            raw.data() + static_cast<std::size_t>(sourceY) * width * bytesPerPixel;
        std::uint8_t* destRow =
            outImage.pixels.data() + static_cast<std::size_t>(y) * width * 4;

        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::uint8_t* src = sourceRow + static_cast<std::size_t>(x) * bytesPerPixel;
            std::uint8_t* dst = destRow + static_cast<std::size_t>(x) * 4;
            dst[0] = src[2]; // R <- B（TGA 是 BGRA 顺序）
            dst[1] = src[1]; // G
            dst[2] = src[0]; // B <- R
            dst[3] = (bytesPerPixel == 4) ? src[3] : 255;
        }
    }

    return true;
}
