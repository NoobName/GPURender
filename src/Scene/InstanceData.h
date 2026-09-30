#pragma once

#include <cstddef>
#include <cstdint>

#include <DirectXMath.h>

// =============================================================================
// GPU 实例数据（Instance Metadata）
// =============================================================================
//
// 这个结构体是 **C++ 与 HLSL 之间的二进制契约**：
// CPU 侧直接按 sizeof(InstanceData) 的步长写进 GPU Buffer，
// HLSL 侧用 StructuredBuffer<InstanceData> 按同样的步长读取。
// 任何一侧的布局改动都会让另一侧读到错位的数据，因此：
//
//   * C++ 侧用 static_assert 把每个字段的偏移与总大小钉死；
//   * HLSL 侧用 sizeof(InstanceData) 在运行期上报自己的计算结果，
//     由 src/Render/InstanceValidator 与 CPU 值比对（见 M9 的验证流程）。
//
// -----------------------------------------------------------------------------
// 内存布局（96 字节）
// -----------------------------------------------------------------------------
//   offset  size  field
//   ------  ----  ----------------------------------------------------------
//        0    64  world           row-major 4x4 矩阵（模型 -> 世界）
//                                 行 0: [m00 m01 m02 m03]  (offset  0..15)
//                                 行 1: [m10 m11 m12 m13]  (offset 16..31)
//                                 行 2: [m20 m21 m22 m23]  (offset 32..47)
//                                 行 3: [m30 m31 m32 m33]  (offset 48..63)  <- 平移分量
//       64    16  boundingSphere  xyz = 世界空间球心，w = 半径
//       80     4  meshIndex       引用场景网格数组
//       84     4  materialIndex   引用场景材质数组
//       88     8  padding[2]      补齐到 96 字节（见下方说明）
//   ------  ----
//       96  total
//
// -----------------------------------------------------------------------------
// 为什么必须补齐到 96 字节（这是 M9 最容易出错的地方）
// -----------------------------------------------------------------------------
// 字段自然相加是 64 + 16 + 4 + 4 = 88 字节，**88 不是 16 的倍数**。
//
// HLSL 的 StructuredBuffer 遵循「紧密布局 + 最大成员对齐」规则：
//   * 成员按自身对齐放置（float4x4 / float4 对齐 16，uint 对齐 4）；
//   * 结构体总大小向上取整到**最大成员对齐**的倍数。
//
// 本结构体最大成员对齐是 16（float4x4），因此 HLSL 算出的
// sizeof(InstanceData) = roundUp(88, 16) = **96**。
//
// 如果 C++ 侧只分配 88 字节而 HLSL 按 96 步进读取，第二个实例开始就会整体错位
// 8 字节 —— 表现为「矩阵读出来是乱的、包围球跑到别的实例身上」，
// 而且**不会报任何错**（D3D12 不做这类检查）。
//
// 所以这里显式写出 padding[2]，让 C++ 的 88+8 与 HLSL 的取整结果 96 对齐。
//
// -----------------------------------------------------------------------------
// 为什么 row-major（XMFLOAT4X4 的原生布局）
// -----------------------------------------------------------------------------
// DirectXMath 的 XMMATRIX / XMFLOAT4X4 本身就是 row-major（行主序），
// XMStoreFloat4x4 直接按行写出。HLSL 读取时用 world[0..3] 取行、
// mul(world, v) 做变换 —— 与 MeshVS.hlsl 的既有约定完全一致，
// 无需转置、无需特殊处理。
// =============================================================================
struct InstanceData
{
    DirectX::XMFLOAT4X4 world;          // offset  0 (64B)
    DirectX::XMFLOAT4 boundingSphere;   // offset 64 (16B)
    std::uint32_t meshIndex;            // offset 80 (4B)
    std::uint32_t materialIndex;        // offset 84 (4B)
    std::uint32_t padding[2];           // offset 88 (8B) -> 补齐到 96
};

// -----------------------------------------------------------------------------
// 显式验证：这些断言就是「C++ 侧布局契约」的可执行形式。
// 任何一条被破坏都会在**编译期**失败，而不是在运行期产生难查的错位。
// -----------------------------------------------------------------------------
static_assert(sizeof(DirectX::XMFLOAT4X4) == 64, "XMFLOAT4X4 必须是 64 字节");
static_assert(sizeof(DirectX::XMFLOAT4) == 16, "XMFLOAT4 必须是 16 字节");

static_assert(offsetof(InstanceData, world) == 0, "world 必须在 offset 0");
static_assert(offsetof(InstanceData, boundingSphere) == 64, "boundingSphere 必须在 offset 64");
static_assert(offsetof(InstanceData, meshIndex) == 80, "meshIndex 必须在 offset 80");
static_assert(offsetof(InstanceData, materialIndex) == 84, "materialIndex 必须在 offset 84");
static_assert(offsetof(InstanceData, padding) == 88, "padding 必须在 offset 88");

static_assert(sizeof(InstanceData) == 96,
              "InstanceData 必须是 96 字节：88 字节的字段 + 8 字节 padding。"
              "HLSL 的 StructuredBuffer 会把 88 向上取整到 16 的倍数（96），"
              "两侧必须一致，否则第二个实例起就会整体错位");

static_assert((sizeof(InstanceData) % 16) == 0,
              "InstanceData 的大小必须是 16 的倍数，才能与 HLSL 的对齐规则一致");
