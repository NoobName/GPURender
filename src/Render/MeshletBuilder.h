#pragma once

#include <cstdint>
#include <vector>

#include <DirectXMath.h>

#include "Asset/MeshData.h"

// =============================================================================
// M17：Meshlet 预处理（加载期 / Asset Preprocessing）
// =============================================================================
// 这里定义 **GPU 侧的 meshlet 数据布局**，并封装对 Microsoft DirectXMesh 的调用。
//
// 为什么把 DirectXMesh 关在 .cpp 里（本文件刻意不 include <DirectXMesh.h>）：
//   DirectXMesh.h 会拉进 <d3d11_4.h> 与一整套内部宏。如果它出现在公开头里，
//   任何 include 本文件的翻译单元都会被污染，第三方依赖也就扩散到了整个项目。
//   所以这里只暴露**我们自己的紧凑 GPU 结构**，转换在 .cpp 里做一次。
//   —— 这样「聚类算法用现成的、数据结构自己掌控」，两边各自干净。
//
// =============================================================================
// 为什么要 Meshlet（一句话）
// =============================================================================
//   传统 whole-mesh 提交的最小单位是「整个 mesh」，剔除也只能整块剔；
//   meshlet 把 mesh 切成 ~128 顶点 / ~128 三角形的小簇，让剔除与 LOD 的
//   粒度从「一个物体」细到「一小块表面」—— 这是 M18 能吃下 Mesh Shader 的前提。
// =============================================================================

// -----------------------------------------------------------------------------
// 一个 meshlet 的描述（stride 16 字节）
// -----------------------------------------------------------------------------
//   VertOffset / VertCount : 指向 **UniqueVertexIndices** 数组的一段
//   PrimOffset / PrimCount : 指向 **PrimitiveIndices** 数组的一段
//
//   注意：**不是**直接用顶点缓冲的索引，而是先查 UniqueVertexIndices。
//   这一层间接是 meshlet 压缩的关键 —— meshlet 内部只用 8 位局部索引
//   （见 MeshletTriGPU），所以必须先通过一张「局部 -> 全局」的映射表还原。
struct MeshletGPU
{
    std::uint32_t vertCount;   // offset  0
    std::uint32_t vertOffset;  // offset  4
    std::uint32_t primCount;   // offset  8
    std::uint32_t primOffset;  // offset 12
};

static_assert(sizeof(MeshletGPU) == 16, "MeshletGPU 必须与 HLSL 的布局一致（stride 16）");

// -----------------------------------------------------------------------------
// 一个 meshlet 的剔除数据（stride 32 字节）
// -----------------------------------------------------------------------------
//   来自 DirectXMesh 的 ComputeCullData。M18 的 amplification 会用它做
//   背面锥剔除与视锥剔除；M17 先用 boundingSphere 画可视化。
struct MeshletBoundsGPU
{
    DirectX::XMFLOAT4 boundingSphere; // offset  0 (16B) xyz = 球心, w = 半径
    std::uint32_t     normalCone;      // offset 16 (4B)  打包的 XMUBYTEN4
                                       //                 xyz = 锥轴(0..255, 偏置 128)
                                       //                 w   = -cos(angle + 90°)
    float             apexOffset;      // offset 20 (4B)  apex = center - axis * offset
    std::uint32_t     pad0;            // offset 24 (4B)
    std::uint32_t     pad1;            // offset 28 (4B)
};

static_assert(sizeof(MeshletBoundsGPU) == 32, "MeshletBoundsGPU 必须与 HLSL 的布局一致（stride 32）");

// -----------------------------------------------------------------------------
// 一个 meshlet 内的三角形（stride 4 字节）
// -----------------------------------------------------------------------------
//   三个 10 位 **局部** 顶点索引，正好覆盖 0..1023，足够 MESHLET_MAXIMUM_SIZE(256)。
//   用 10+10+10 打包后，一个三角形只占 4 字节 —— 相比三个 uint32 省 2/3 带宽。
//
//   最高 2 位闲置（0..1023 只需 10 位），保留给将来可能的材质/属性标记。
struct MeshletTriGPU
{
    std::uint32_t i0 : 10;
    std::uint32_t i1 : 10;
    std::uint32_t i2 : 10;
    std::uint32_t    : 2;  // 显式占位，保证 sizeof == 4
};

static_assert(sizeof(MeshletTriGPU) == 4, "MeshletTriGPU 必须是紧凑的 4 字节（10+10+10 位打包）");

// -----------------------------------------------------------------------------
// 预处理统计（验收要求「输出 Meshlet Count」）
// -----------------------------------------------------------------------------
struct MeshletBuildStats
{
    std::uint32_t meshletCount            = 0; // 拆分出的 meshlet 数量
    std::uint32_t uniqueVertexIndexCount  = 0; // UniqueVertexIndices 的总元素数
    std::uint32_t primitiveIndexCount     = 0; // PrimitiveIndices 的总元素数
    std::uint32_t sourceVertexCount       = 0; // 原始网格顶点数
    std::uint32_t sourceTriangleCount     = 0; // 原始网格三角形数

    std::uint32_t maxVertsPerMeshlet      = 0;
    std::uint32_t maxPrimsPerMeshlet      = 0;
    std::uint32_t minVertsPerMeshlet      = 0;

    // 顶点重复率 = uniqueVertexIndexCount / sourceVertexCount。
    //   1.0 表示每个顶点恰好被一个 meshlet 引用（理想，但现实中边界顶点必然重复）。
    //   这个数字直接说明「meshlet 划分带来了多少顶点冗余」——是面试常问的指标。
    float         vertexDuplication       = 0.0f;

    // 被 >1 个 meshlet 引用的顶点数（即切开处的顶点）。
    std::uint32_t boundaryVertexCount     = 0;
};

// =============================================================================
// MeshletBuilder：在**加载期**调用 DirectXMesh 生成 meshlet
// =============================================================================
// 用 Microsoft DirectXMesh 的 ComputeMeshlets + ComputeCullData，
// **不自己实现聚类算法**（用户明确要求）。
//
// 生成结果仅存在于 CPU 侧；上传 GPU 由 MeshletResources 负责 ——
// 两件事分开，是因为「预处理」与「资源生命周期」是不同关注点。
// =============================================================================
class MeshletBuilder
{
public:
    // meshlet 规模上限。
    //
    // 128/128 是 DirectXMesh 的默认值，也是行业常见选择：
    //   * 顶点数上界决定 meshlet 内部索引能塞进 10 位（1024）；
    //   * 128 个顶点约等于「顶点缓冲一次能覆盖的邻域」，硬件缓存友好；
    //   * 三角形数上界 128 让一个 meshlet 恰好是一个 SM 波前附近的粒度。
    static constexpr std::uint32_t kDefaultMaxVerts = 128;
    static constexpr std::uint32_t kDefaultMaxPrims = 128;

    // 从网格数据生成 meshlet。失败时返回 false 并打印原因。
    //   mesh 必须已展开（顶点属性各不相同），否则 DirectXMesh 会因共享顶点报错。
    bool Build(const MeshData& mesh,
               std::uint32_t maxVerts = kDefaultMaxVerts,
               std::uint32_t maxPrims = kDefaultMaxPrims);

    const std::vector<MeshletGPU>&               GetMeshlets() const { return m_meshlets; }
    const std::vector<std::uint32_t>&            GetUniqueVertexIndices() const { return m_uniqueVertexIndices; }
    const std::vector<MeshletTriGPU>&            GetPrimitiveIndices() const { return m_primitiveIndices; }
    const std::vector<MeshletBoundsGPU>&         GetBounds() const { return m_bounds; }
    const MeshletBuildStats&                     GetStats() const { return m_stats; }

    bool IsBuilt() const { return !m_meshlets.empty(); }

private:
    std::vector<MeshletGPU>        m_meshlets;
    std::vector<std::uint32_t>     m_uniqueVertexIndices;
    std::vector<MeshletTriGPU>     m_primitiveIndices;
    std::vector<MeshletBoundsGPU>  m_bounds;
    MeshletBuildStats              m_stats;
};
