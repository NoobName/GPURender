#include "Render/MeshLOD.h"

#include <cmath>

namespace
{
// 立方体的一个面。用「原点 + 两个边向量」描述，便于按 N×N 采样。
//
//   position(u, v) = origin + du * u + dv * v     u, v ∈ [0, 1]
//
// 六个面的绕序都取**逆时针**（从面外侧看），与项目一直使用的
// `FrontCounterClockwise = FALSE` + `CullMode = NONE` 兼容 ——
// 这里不依赖剔除，但保持绕序一致可以让将来开启背面剔除时不用改数据。
struct CubeFace
{
    float origin[3];
    float du[3];
    float dv[3];
    float normal[3];
};

constexpr CubeFace kFaces[6] = {
    // +X
    { {  1.0f, -1.0f, -1.0f }, {  0.0f, 0.0f,  2.0f }, { 0.0f, 2.0f, 0.0f }, {  1.0f,  0.0f,  0.0f } },
    // -X
    { { -1.0f, -1.0f,  1.0f }, {  0.0f, 0.0f, -2.0f }, { 0.0f, 2.0f, 0.0f }, { -1.0f,  0.0f,  0.0f } },
    // +Y
    { { -1.0f,  1.0f, -1.0f }, {  2.0f, 0.0f,  0.0f }, { 0.0f, 0.0f, 2.0f }, {  0.0f,  1.0f,  0.0f } },
    // -Y
    { { -1.0f, -1.0f,  1.0f }, {  2.0f, 0.0f,  0.0f }, { 0.0f, 0.0f, -2.0f }, {  0.0f, -1.0f,  0.0f } },
    // +Z
    { { -1.0f, -1.0f,  1.0f }, {  2.0f, 0.0f,  0.0f }, { 0.0f, 2.0f, 0.0f }, {  0.0f,  0.0f,  1.0f } },
    // -Z
    { {  1.0f, -1.0f, -1.0f }, { -2.0f, 0.0f,  0.0f }, { 0.0f, 2.0f, 0.0f }, {  0.0f,  0.0f, -1.0f } },
};

// 把一级细分立方体追加进合并数据，并回填它在该级里的范围。
void AppendLODLevel(MeshData& data,
                    MeshLODRange& range,
                    std::uint32_t segments)
{
    const std::uint32_t vertexBase = static_cast<std::uint32_t>(data.vertices.size());
    const std::uint32_t indexBase = static_cast<std::uint32_t>(data.indices.size());

    range.baseVertex = static_cast<std::int32_t>(vertexBase);
    range.indexOffset = indexBase;

    const std::uint32_t quads = segments * segments;

    for (const CubeFace& face : kFaces)
    {
        const std::uint32_t faceVertexBase = static_cast<std::uint32_t>(data.vertices.size());

        // 顶点：每面 (N+1)^2 个。
        //
        // **不跨面共享顶点**：立方体的棱上两条法线不同，共享会让光照在棱处
        // 被插值糊掉。多出的顶点数量相对于三角形数可以忽略。
        for (std::uint32_t y = 0; y <= segments; ++y)
        {
            const float v = static_cast<float>(y) / static_cast<float>(segments);
            for (std::uint32_t x = 0; x <= segments; ++x)
            {
                const float u = static_cast<float>(x) / static_cast<float>(segments);

                MeshVertex vertex = {};
                vertex.position.x = face.origin[0] + face.du[0] * u + face.dv[0] * v;
                vertex.position.y = face.origin[1] + face.du[1] * u + face.dv[1] * v;
                vertex.position.z = face.origin[2] + face.du[2] * u + face.dv[2] * v;
                vertex.normal.x = face.normal[0];
                vertex.normal.y = face.normal[1];
                vertex.normal.z = face.normal[2];
                vertex.uv.x = u;
                vertex.uv.y = 1.0f - v; // 让 v 向上时 uv.y 也向上，便于观察贴图方向

                data.vertices.push_back(vertex);
            }
        }

        for (std::uint32_t y = 0; y < segments; ++y)
        {
            for (std::uint32_t x = 0; x < segments; ++x)
            {
                const std::uint32_t rowStride = segments + 1u;
                const std::uint32_t i0 = faceVertexBase + y * rowStride + x;
                const std::uint32_t i1 = i0 + 1u;
                const std::uint32_t i2 = i0 + rowStride;
                const std::uint32_t i3 = i2 + 1u;

                data.indices.push_back(i0);
                data.indices.push_back(i2);
                data.indices.push_back(i1);

                data.indices.push_back(i1);
                data.indices.push_back(i2);
                data.indices.push_back(i3);
            }
        }
    }

    range.indexCount = static_cast<std::uint32_t>(data.indices.size()) - indexBase;
    range.vertexCount = static_cast<std::uint32_t>(data.vertices.size()) - vertexBase;
    range.triangleCount = range.indexCount / 3u;

    // 每面 quads 个四边形、每个 2 个三角形、共 6 个面
    (void)quads;
}
} // namespace

MeshData BuildSubdividedCubeLODChain(const std::vector<std::uint32_t>& segmentsPerLevel,
                                     const std::vector<float>& thresholds,
                                     std::vector<MeshLODRange>& outRanges)
{
    MeshData data;
    outRanges.clear();

    const std::size_t levelCount = segmentsPerLevel.size();
    outRanges.resize(levelCount);

    for (std::size_t level = 0; level < levelCount; ++level)
    {
        AppendLODLevel(data, outRanges[level], segmentsPerLevel[level]);

        // 最后一级没有「更粗的一级」，阈值置 0（永远不会被用来切换）
        const float threshold = (level + 1 < levelCount && level < thresholds.size())
                                    ? thresholds[level]
                                    : 0.0f;
        outRanges[level].screenSizeThreshold = threshold;
    }

    return data;
}
