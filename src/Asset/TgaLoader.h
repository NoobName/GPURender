#pragma once

#include <cstdint>
#include <string>
#include <vector>

// CPU 侧的图像数据：8 位 RGBA、行优先、**左上角为原点**。
// 统一成这一种表示后，上传代码就不必再关心源文件的具体像素布局。
struct ImageData
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels; // RGBA8，长度 = width * height * 4
};

// 极简 TGA 解码器。
//
// 支持：imageType == 2（未压缩真彩色），24 位 BGR 与 32 位 BGRA，
//       以及两种原点（通过 imageDescriptor 的 bit5 判断，统一翻转成左上原点）。
// 不支持：RLE 压缩（imageType 10）、调色板、灰度 —— 本项目用不到。
//
// TGA 头部只有 18 字节且不需要 zlib，因此这个解码器很短；
// 这样能把精力留在真正重要的地方：纹理从磁盘到显存、再到 SRV 与采样器这条链路。
bool LoadTgaFromFile(const std::string& path, ImageData& outImage, std::string& outError);
