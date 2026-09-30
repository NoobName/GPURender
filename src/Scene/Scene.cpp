#include "Scene/Scene.h"

#include <cmath>

namespace
{
// 确定性 PRNG（SplitMix64）。
//
// 为什么不用 std::mt19937 + std::uniform_real_distribution：
//   引擎本身的输出是标准化的，但 <random> 的**分布**实现（uniform_real_distribution）
//   并不保证跨标准库逐位一致。Benchmark 场景要求「同种子 -> 同场景」，
//   所以这里用一个只有几行的、行为完全确定的生成器，配合手写的 [0,1) 映射。
struct SplitMix64
{
    std::uint64_t state;

    explicit SplitMix64(std::uint64_t seed) : state(seed) {}

    std::uint64_t Next()
    {
        state += 0x9E3779B97F4A7C15ull; // 黄金比例常数，SplitMix 的标准增量
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // 取高 24 位映射到 [0,1)，避免低位质量较差的位影响结果。
    float NextFloat()
    {
        return static_cast<float>(Next() >> 40) * (1.0f / 16777216.0f);
    }
};

constexpr float kPi = 3.14159265358979323846f;

// 把平面归一化，使「球心到平面的距离」与「球半径」可以直接比较。
void NormalizePlane(DirectX::XMFLOAT4& plane)
{
    const float lengthSq = plane.x * plane.x + plane.y * plane.y + plane.z * plane.z;
    if (lengthSq > 0.0f)
    {
        const float invLength = 1.0f / std::sqrt(lengthSq);
        plane.x *= invLength;
        plane.y *= invLength;
        plane.z *= invLength;
        plane.w *= invLength;
    }
}
} // namespace

void Scene::Generate(const SceneConfig& config,
                     float meshLocalRadius,
                     std::uint32_t meshCount,
                     std::uint32_t materialCount)
{
    m_config = config;
    m_instances.clear();
    m_instances.reserve(config.instanceCount);

    SplitMix64 rng(config.randomSeed);

    for (std::uint32_t i = 0; i < config.instanceCount; ++i)
    {
        // --- 位置：球体内**体积均匀**分布 ---
        // 半径要按 cbrt(u) 取而不是 u，否则实例会明显向球心聚集
        // （因为半径 r 处的球壳体积正比于 r^2）。
        const float u = rng.NextFloat();
        const float radius = config.fieldRadius * std::cbrt(u);

        // 球面均匀方向：cos(theta) 在 [-1,1] 上均匀，才能保证面积均匀。
        const float cosTheta = 1.0f - 2.0f * rng.NextFloat();
        const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
        const float phi = 2.0f * kPi * rng.NextFloat();

        const DirectX::XMFLOAT3 position = {
            radius * sinTheta * std::cos(phi),
            radius * cosTheta * config.fieldFlatten,
            radius * sinTheta * std::sin(phi),
        };

        const float scale = config.minScale +
                            (config.maxScale - config.minScale) * rng.NextFloat();

        const float rotationX = 2.0f * kPi * rng.NextFloat();
        const float rotationY = 2.0f * kPi * rng.NextFloat();
        const float rotationZ = 2.0f * kPi * rng.NextFloat();

        // row-vector 约定：先缩放，再旋转，最后平移到世界位置。
        const DirectX::XMMATRIX world =
            DirectX::XMMatrixScaling(scale, scale, scale) *
            DirectX::XMMatrixRotationRollPitchYaw(rotationX, rotationY, rotationZ) *
            DirectX::XMMatrixTranslation(position.x, position.y, position.z);

        InstanceData instance = {};
        DirectX::XMStoreFloat4x4(&instance.world, world);

        // 包围球：球心就是实例中心（缩放/旋转都绕中心，不改变中心位置），
        // 半径 = 网格局部半径 * 缩放。
        instance.boundingSphere = { position.x, position.y, position.z, meshLocalRadius * scale };

        instance.meshIndex = (meshCount > 0) ? static_cast<std::uint32_t>(rng.Next() % meshCount) : 0;
        instance.materialIndex = (materialCount > 0)
                                     ? static_cast<std::uint32_t>(rng.Next() % materialCount)
                                     : 0;

        m_instances.push_back(instance);
    }
}

void ExtractFrustumPlanes(DirectX::FXMMATRIX viewProj, DirectX::XMFLOAT4 outPlanes[6])
{
    DirectX::XMFLOAT4X4 m;
    DirectX::XMStoreFloat4x4(&m, viewProj);

    // 从 viewProj 提取 6 个裁剪平面（Gribb-Hartmann 方法）。
    //
    // 推导（row-vector 约定，clip = v * M）：
    //   clip.x = v · col0,  clip.w = v · col3
    //   左平面条件 clip.x >= -clip.w  =>  (col0 + col3) · v >= 0
    // 其余平面同理。D3D 的 NDC 是 x,y ∈ [-1,1]、z ∈ [0,1]，
    // 所以近平面直接用 col2（z >= 0），远平面是 col3 - col2（z <= w）。
    //
    // 注意 D3D 与 OpenGL 的差异：OpenGL 的 NDC z ∈ [-1,1]，
    // 近平面会变成 col2 + col3。搞错这一条会让近平面判断完全失效。
    const DirectX::XMFLOAT4 col0 = { m.m[0][0], m.m[1][0], m.m[2][0], m.m[3][0] };
    const DirectX::XMFLOAT4 col1 = { m.m[0][1], m.m[1][1], m.m[2][1], m.m[3][1] };
    const DirectX::XMFLOAT4 col2 = { m.m[0][2], m.m[1][2], m.m[2][2], m.m[3][2] };
    const DirectX::XMFLOAT4 col3 = { m.m[0][3], m.m[1][3], m.m[2][3], m.m[3][3] };

    outPlanes[0] = { col0.x + col3.x, col0.y + col3.y, col0.z + col3.z, col0.w + col3.w }; // left
    outPlanes[1] = { col3.x - col0.x, col3.y - col0.y, col3.z - col0.z, col3.w - col0.w }; // right
    outPlanes[2] = { col1.x + col3.x, col1.y + col3.y, col1.z + col3.z, col1.w + col3.w }; // bottom
    outPlanes[3] = { col3.x - col1.x, col3.y - col1.y, col3.z - col1.z, col3.w - col1.w }; // top
    outPlanes[4] = col2;                                                                  // near
    outPlanes[5] = { col3.x - col2.x, col3.y - col2.y, col3.z - col2.z, col3.w - col2.w }; // far

    for (std::uint32_t i = 0; i < 6; ++i)
    {
        NormalizePlane(outPlanes[i]);
    }
}

void CullInstancesByFrustumWithPlanes(const std::vector<InstanceData>& instances,
                                      const DirectX::XMFLOAT4 planes[6],
                                      std::vector<std::uint32_t>& outVisibleIndices)
{
    outVisibleIndices.clear();
    outVisibleIndices.reserve(instances.size());

    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(instances.size()); ++i)
    {
        const DirectX::XMFLOAT4& sphere = instances[i].boundingSphere;
        const float radius = sphere.w;

        // 保守测试：只有当球**完全**落在某个平面外侧时才判定不可见。
        // 这样不会误剔除，代价是可能保留少量实际不可见的实例 —— 对剔除来说是正确的取舍。
        //
        // 这里的算术写法刻意与 FrustumCullingCS.hlsl 保持一致
        // （逐个乘加，而不是 dot()），让两侧尽可能生成相同的浮点运算序列。
        bool visible = true;
        for (std::uint32_t planeIndex = 0; planeIndex < 6; ++planeIndex)
        {
            const DirectX::XMFLOAT4& plane = planes[planeIndex];
            const float distance =
                plane.x * sphere.x + plane.y * sphere.y + plane.z * sphere.z + plane.w;
            if (distance < -radius)
            {
                visible = false;
                break;
            }
        }

        if (visible)
        {
            outVisibleIndices.push_back(i);
        }
    }
}

void CullInstancesByFrustum(const std::vector<InstanceData>& instances,
                            DirectX::FXMMATRIX viewProj,
                            std::vector<std::uint32_t>& outVisibleIndices)
{
    DirectX::XMFLOAT4 planes[6];
    ExtractFrustumPlanes(viewProj, planes);
    CullInstancesByFrustumWithPlanes(instances, planes, outVisibleIndices);
}
