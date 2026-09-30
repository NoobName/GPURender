#pragma once

#include <cstdint>
#include <vector>

#include <DirectXMath.h>

// CPU 侧的顶点格式：position + normal + uv。
//
// 为什么放在 Asset 层而不是 Render 层：
//   它同时是「资源格式」和「GPU 输入布局」的约定 —— 文件解析器产出它，
//   渲染层消费它。放在中间这一层可以避免两层互相依赖。
//
// 内存布局（32 字节，无隐式 padding）：
//   offset  0 : position (float3, 12 字节)
//   offset 12 : normal   (float3, 12 字节)
//   offset 24 : uv       (float2,  8 字节)
// 因为布局是确定的，GPU 的 Input Layout 偏移可以直接写 0 / 12 / 24，
// 与 C++ 侧结构体完全一致 —— 这正是 M5 那个「偏移对不上」类 bug 的根治办法。
struct MeshVertex
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 uv;
};

static_assert(sizeof(MeshVertex) == 32,
              "MeshVertex 必须是紧凑的 32 字节：GPU Input Layout 的 0/12/24 偏移依赖它");

// CPU 侧的网格数据（还未上传到 GPU）。
// 保持极简：一个顶点数组 + 一个 32 位索引数组，足以描述 M6 需要的所有网格。
struct MeshData
{
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
};
