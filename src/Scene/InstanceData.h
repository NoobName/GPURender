#pragma once

#include <cstdint>

#include <DirectXMath.h>

// 一个可绘制实例的完整描述（与 GPU API 无关的扁平数据）。
//
// 为什么把 bounding sphere 也放进来：
//   视锥剔除 / 遮挡剔除 / LOD 选择都需要一个「包围体」来做廉价判定。
//   M7 只用它做 CPU 视锥测试，但同一份数据在 M8 之后会被直接打包进 GPU 缓冲
//   交给 Compute Shader 读 —— 所以这里刻意把结构设计成「GPU 友好」的扁平布局，
//   而不是塞进一堆 C++ 对象指针。
//
// 为什么是 96 字节：
//   64（矩阵）+ 16（球）+ 4 + 4 = 88，不是 16 的倍数。
//   HLSL 的 StructuredBuffer 每个元素要求 16 字节对齐，所以补齐到 96，
//   避免 M8 打包时再回头改结构体。
//
// 内存布局：
//   offset  0 : world           float4x4 -> 模型 -> 世界
//   offset 64 : boundingSphere  float4   -> xyz = 世界空间球心，w = 半径
//   offset 80 : meshIndex       uint
//   offset 84 : materialIndex   uint
//   offset 88 : padding[2]      uint     -> 补齐到 96
struct InstanceData
{
    DirectX::XMFLOAT4X4 world;
    DirectX::XMFLOAT4 boundingSphere;
    std::uint32_t meshIndex;
    std::uint32_t materialIndex;
    std::uint32_t padding[2];
};

static_assert(sizeof(InstanceData) == 96,
              "InstanceData 必须紧凑地占 96 字节：M8 打包进 StructuredBuffer 时依赖它");
