#pragma once

#include <cstdint>
#include <vector>

#include "Asset/MeshData.h"

// GPU-Driven LOD：网格的多级细节（M16）。
//
// =============================================================================
// 为什么要「一个缓冲装下所有 LOD」
// =============================================================================
//   间接绘制命令里能表达几何选择的字段只有三个：
//       IndexCountPerInstance / StartIndexLocation / BaseVertexLocation
//   因此最自然的布局就是把所有 LOD 的顶点与索引**顺序拼进同一个缓冲**，
//   每一级用 (StartIndexLocation, BaseVertexLocation) 指向自己的那一段：
//
//       vertexBuffer:  [ LOD0 顶点 ][ LOD1 顶点 ][ LOD2 顶点 ][ LOD3 顶点 ]
//       indexBuffer:   [ LOD0 索引 ][ LOD1 索引 ][ LOD2 索引 ][ LOD3 索引 ]
//
//   于是「选 LOD」这件事就退化成「往命令里写不同的偏移」——
//   完全可以在 GPU 上完成，不需要任何 CPU 参与，也不需要切换 PSO/缓冲绑定。
//
//   > 对比一下另一条路：把每个 LOD 做成独立资源，那就得为每级分别绑定
//   > 顶点/索引缓冲，CPU 必须知道有几级、哪一级画谁 —— 违背了
//   > 「GPU 决定画什么」的前提。
//
// =============================================================================
// 本项目的 LOD 几何：细分立方体
// =============================================================================
//   场景里的实例是立方体（M7 起一直如此），所以直接用**同一个立方体的
//   不同细分级别**做 LOD 链，视觉上是连续的：
//
//       LOD0  每面 8x8  ->  6 * 8*8*2  = 768 三角形
//       LOD1  每面 4x4  ->  6 * 4*4*2  = 192
//       LOD2  每面 2x2  ->  6 * 2*2*2  =  48
//       LOD3  每面 1x1  ->  6 * 1*1*2  =  12   ← 就是原来的立方体
//
//   为什么不直接在 sphere.obj 上做简化：那需要一个网格简化算法
//   （边折叠等），复杂度远高于本阶段要讲清楚的东西 —— 「GPU 按投影尺寸选 LOD」。
//   程序化细分立方体让 LOD 链是**解析可控**的，便于精确验证每一级的几何正确性。
//
// =============================================================================
// 屏幕尺寸阈值
// =============================================================================
//   阈值的度量是「包围球投影后的直径占屏幕高度的比例」（0..1）：
//
//       screenSize = 2 * r_ndc.y        （r_ndc.y 是 NDC 空间的 y 半径）
//
//   选择**占屏幕比例**而不是像素数：这样阈值与分辨率无关，
//   换分辨率时不需要重新调参。这也是它优于「世界空间距离」的核心原因之一。
//
//   语义：screenSize < threshold[i] 时切到第 i+1 级。
//   最后一级的 threshold 无意义（没有更粗的了）。
struct MeshLODRange
{
    std::uint32_t indexOffset = 0;         // 在合并索引缓冲里的起始索引
    std::uint32_t indexCount = 0;
    std::int32_t baseVertex = 0;           // 该级顶点在合并顶点缓冲里的基址
    std::uint32_t triangleCount = 0;

    float screenSizeThreshold = 0.0f;      // 低于此屏幕尺寸 -> 切到下一级
    std::uint32_t vertexCount = 0;
    std::uint32_t pad0 = 0;
    std::uint32_t pad1 = 0;
};

// GPU 侧的 StructuredBuffer<MeshLODRange> 依赖这个紧凑布局（stride = 32）。
// 任何字段变动都会让 shader 读到错位的数据 —— 与 M9 的 InstanceData 同样的理由。
static_assert(sizeof(MeshLODRange) == 32,
              "MeshLODRange must stay 32 bytes: the HLSL struct layout depends on it");

// 生成一条细分立方体的 LOD 链，并把所有级别拼进一个 MeshData。
//
//   segmentsPerLevel : 每级「每个面的细分数」，例如 { 8, 4, 2, 1 }
//   thresholds       : 每级的屏幕尺寸阈值（最后一个元素不使用）
//
// outRanges 的每个元素描述该级在合并缓冲里的 (indexOffset, baseVertex, ...)。
MeshData BuildSubdividedCubeLODChain(const std::vector<std::uint32_t>& segmentsPerLevel,
                                     const std::vector<float>& thresholds,
                                     std::vector<MeshLODRange>& outRanges);
