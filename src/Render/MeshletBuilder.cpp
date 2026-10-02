#include "Render/MeshletBuilder.h"

#include <algorithm>
#include <cstring>
#include <iostream>

// DirectXMesh 只在这个 .cpp 里出现 —— 公开头（MeshletBuilder.h）不暴露它，
// 第三方依赖因此被限制在这一个翻译单元内。
#include <DirectXMesh.h>

namespace
{
// DirectXMesh 的 MeshletTriangle 与我们的 MeshletTriGPU 是同一套 10+10+10 位打包。
// 两边都 static_assert 成 4 字节，所以可以整块内存拷贝 —— 比逐字段拆位再打包快得多，
// 而且不会引入「位序理解错误」这类隐蔽 bug。
static_assert(sizeof(DirectX::MeshletTriangle) == sizeof(MeshletTriGPU),
              "DirectXMesh 的 MeshletTriangle 布局变了，需要重新检查转换逻辑");
} // namespace

bool MeshletBuilder::Build(const MeshData& mesh, std::uint32_t maxVerts, std::uint32_t maxPrims)
{
    m_meshlets.clear();
    m_uniqueVertexIndices.clear();
    m_primitiveIndices.clear();
    m_bounds.clear();
    m_stats = {};

    // ---- 输入校验 -----------------------------------------------------------
    if (mesh.vertices.empty() || mesh.indices.empty())
    {
        std::cerr << "[MeshletBuilder] Mesh is empty.\n";
        return false;
    }

    if ((mesh.indices.size() % 3u) != 0u)
    {
        std::cerr << "[MeshletBuilder] Index count is not a multiple of 3.\n";
        return false;
    }

    const std::size_t nFaces = mesh.indices.size() / 3u;
    const std::size_t nVerts = mesh.vertices.size();

    // DirectXMesh 要的是**纯位置数组**，而我们的顶点是 position/normal/uv 交织的 32 字节。
    // 所以先抽一份紧凑的 XMFLOAT3 数组出来。
    //
    // 这份临时数组在整个 Build 期间都活着（ComputeCullData 也要用），
    // 是必要的代价 —— 它让 DirectXMesh 的接口保持原始形态，不需要我们改它的代码。
    std::vector<DirectX::XMFLOAT3> positions(nVerts);
    for (std::size_t i = 0; i < nVerts; ++i)
    {
        positions[i] = mesh.vertices[i].position;
    }

    // ---- 1. 生成 meshlet ----------------------------------------------------
    //
    // adjacency 传 nullptr：让 DirectXMesh 内部调用 GenerateAdjacencyAndPointReps
    // 自己算邻接。这正是我们链接 DirectXMeshAdjacency.cpp 的原因。
    //
    // **聚类算法完全由 DirectXMesh 提供**（贪心 + 空间局部性/朝向一致性/顶点复用
    // 三项加权评分），本项目不实现任何自己的聚类逻辑。
    std::vector<DirectX::Meshlet>         meshlets;
    std::vector<std::uint8_t>             uniqueVertexIB; // 原始字节：内容是 uint32 顶点索引
    std::vector<DirectX::MeshletTriangle> primitiveIndices;

    HRESULT hr = DirectX::ComputeMeshlets(mesh.indices.data(),
                                          nFaces,
                                          positions.data(),
                                          nVerts,
                                          nullptr, // adjacency -> 内部自动生成
                                          meshlets,
                                          uniqueVertexIB,
                                          primitiveIndices,
                                          static_cast<std::size_t>(maxVerts),
                                          static_cast<std::size_t>(maxPrims));
    if (FAILED(hr))
    {
        std::cerr << "[MeshletBuilder] ComputeMeshlets failed (hr=0x" << std::hex << hr << std::dec << ").\n";
        return false;
    }

    if (meshlets.empty())
    {
        std::cerr << "[MeshletBuilder] ComputeMeshlets produced no meshlets.\n";
        return false;
    }

    // uniqueVertexIB 的字节数是「元素数 × sizeof(uint32_t)」——因为上面调的是
    // uint32_t 索引的重载。把它看成一串 uint32 才是正确的读法。
    const std::size_t uniqueVertexCount = uniqueVertexIB.size() / sizeof(std::uint32_t);
    const std::uint32_t* uniqueVertexIndices =
        reinterpret_cast<const std::uint32_t*>(uniqueVertexIB.data());

    // ---- 2. 生成剔除数据（包围球 + 法线锥）---------------------------------
    //
    // 要求 3「Meshlet Culling Metadata」以及「如果 DirectXMesh 支持，保存
    // Bounding Sphere / Normal Cone」的落点 —— DirectXMesh 两者都提供。
    std::vector<DirectX::CullData> cullData(meshlets.size());

    hr = DirectX::ComputeCullData(positions.data(),
                                  nVerts,
                                  meshlets.data(),
                                  meshlets.size(),
                                  uniqueVertexIndices,
                                  uniqueVertexCount,
                                  primitiveIndices.data(),
                                  primitiveIndices.size(),
                                  cullData.data(),
                                  DirectX::MESHLET_DEFAULT);
    if (FAILED(hr))
    {
        std::cerr << "[MeshletBuilder] ComputeCullData failed (hr=0x" << std::hex << hr << std::dec << ").\n";
        return false;
    }

    // ---- 3. 转换到我们自己的 GPU 布局 --------------------------------------
    m_meshlets.resize(meshlets.size());
    for (std::size_t i = 0; i < meshlets.size(); ++i)
    {
        m_meshlets[i].vertCount  = meshlets[i].VertCount;
        m_meshlets[i].vertOffset = meshlets[i].VertOffset;
        m_meshlets[i].primCount  = meshlets[i].PrimCount;
        m_meshlets[i].primOffset = meshlets[i].PrimOffset;
    }

    m_uniqueVertexIndices.assign(uniqueVertexIndices, uniqueVertexIndices + uniqueVertexCount);

    // 位布局一致 -> 整块拷贝（见文件顶部的 static_assert）
    m_primitiveIndices.resize(primitiveIndices.size());
    std::memcpy(m_primitiveIndices.data(),
                primitiveIndices.data(),
                primitiveIndices.size() * sizeof(MeshletTriGPU));

    m_bounds.resize(cullData.size());
    for (std::size_t i = 0; i < cullData.size(); ++i)
    {
        const DirectX::CullData& src = cullData[i];

        m_bounds[i].boundingSphere.x = src.BoundingSphere.Center.x;
        m_bounds[i].boundingSphere.y = src.BoundingSphere.Center.y;
        m_bounds[i].boundingSphere.z = src.BoundingSphere.Center.z;
        m_bounds[i].boundingSphere.w = src.BoundingSphere.Radius;

        // NormalCone 是 XMUBYTEN4（4 个 uint8 打包进一个 uint32）。
        // 逐字节搬运，避免依赖 PackedVector 的联合体写法。
        std::uint32_t packedCone = 0;
        std::memcpy(&packedCone, &src.NormalCone, sizeof(std::uint32_t));
        m_bounds[i].normalCone = packedCone;

        m_bounds[i].apexOffset = src.ApexOffset;
        m_bounds[i].pad0       = 0;
        m_bounds[i].pad1       = 0;
    }

    // ---- 4. 统计 ------------------------------------------------------------
    m_stats.meshletCount           = static_cast<std::uint32_t>(m_meshlets.size());
    m_stats.uniqueVertexIndexCount = static_cast<std::uint32_t>(uniqueVertexCount);
    m_stats.primitiveIndexCount    = static_cast<std::uint32_t>(m_primitiveIndices.size());
    m_stats.sourceVertexCount      = static_cast<std::uint32_t>(nVerts);
    m_stats.sourceTriangleCount    = static_cast<std::uint32_t>(nFaces);

    m_stats.minVertsPerMeshlet = UINT32_MAX;
    for (const MeshletGPU& m : m_meshlets)
    {
        m_stats.maxVertsPerMeshlet = (std::max)(m_stats.maxVertsPerMeshlet, m.vertCount);
        m_stats.maxPrimsPerMeshlet = (std::max)(m_stats.maxPrimsPerMeshlet, m.primCount);
        m_stats.minVertsPerMeshlet = (std::min)(m_stats.minVertsPerMeshlet, m.vertCount);
    }

    // 顶点重复率与边界顶点数。
    //
    // uniqueVertexIndices 里存的是**原始网格顶点索引**，所以直接对源顶点做计数即可：
    // 引用次数 > 1 的顶点就是被 cut 到多个 meshlet 的边界顶点。
    // 这个数字说明 meshlet 划分的代价 —— 边界越多，顶点着色器要重复计算的越多。
    {
        std::vector<std::uint32_t> refCount(nVerts, 0u);
        std::uint32_t boundary = 0;

        for (std::uint32_t v : m_uniqueVertexIndices)
        {
            if (v < nVerts)
            {
                ++refCount[v];
            }
        }
        for (std::uint32_t c : refCount)
        {
            if (c > 1u)
            {
                ++boundary;
            }
        }

        m_stats.boundaryVertexCount = boundary;
        m_stats.vertexDuplication =
            (nVerts > 0) ? (static_cast<float>(uniqueVertexCount) / static_cast<float>(nVerts)) : 0.0f;
    }

    // ---- 5. 输出（验收要求「输出 Meshlet Count」）--------------------------
    std::cout << "[MeshletBuilder] Meshlet preprocessing done (DirectXMesh)\n";
    std::cout << "        source mesh      : " << nVerts << " verts / " << nFaces << " tris\n";
    std::cout << "        meshlet count    : " << m_stats.meshletCount << "\n";
    std::cout << "        verts per meshlet: min " << m_stats.minVertsPerMeshlet
              << " / max " << m_stats.maxVertsPerMeshlet << "  (limit " << maxVerts << ")\n";
    std::cout << "        prims per meshlet: max " << m_stats.maxPrimsPerMeshlet
              << "  (limit " << maxPrims << ")\n";
    std::cout << "        unique vert refs : " << m_stats.uniqueVertexIndexCount
              << "  (duplication x" << m_stats.vertexDuplication << ")\n";
    std::cout << "        boundary verts   : " << m_stats.boundaryVertexCount
              << "  (referenced by >1 meshlet)\n";
    std::cout << "        primitive indices: " << m_stats.primitiveIndexCount
              << "  (" << (sizeof(MeshletTriGPU)) << " B each, 10+10+10 bit packed)\n";

    return true;
}
