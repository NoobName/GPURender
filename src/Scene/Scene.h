#pragma once

#include <cstdint>
#include <vector>

#include <DirectXMath.h>

#include "Scene/InstanceData.h"

// 测试场景的配置。修改 instanceCount 就能在 1,000 / 10,000 / 100,000 之间切换规模。
struct SceneConfig
{
    std::uint32_t instanceCount = 1000;
    std::uint32_t randomSeed = 0x5EED1234u; // 固定种子 -> 确定性场景
    float fieldRadius = 55.0f;              // 实例分布在一个球形空间内
    float fieldFlatten = 0.55f;             // Y 方向压扁，让场景更像一片「地面附近」的物体
    float minScale = 0.5f;
    float maxScale = 1.8f;
};

// 场景：一组 InstanceData，外加生成它的配置。
//
// M7 的场景是**静态且确定性**的：同一个 SceneConfig 一定生成逐字节相同的 instance 数据。
// 这既是 benchmark 可复现的前提，也是后续「CPU baseline vs GPU-Driven」公平对比的基础 ——
// 如果每次运行的场景都不一样，两边的耗时就没有可比性。
class Scene
{
public:
    // 生成确定性测试场景。
    //   meshLocalRadius : 网格在局部空间的包围球半径（用于算每个实例的世界包围球）
    //   meshCount / materialCount : 每个实例随机引用其中之一
    void Generate(const SceneConfig& config,
                  float meshLocalRadius,
                  std::uint32_t meshCount,
                  std::uint32_t materialCount);

    const std::vector<InstanceData>& GetInstances() const { return m_instances; }
    const SceneConfig& GetConfig() const { return m_config; }

private:
    SceneConfig m_config;
    std::vector<InstanceData> m_instances;
};

// CPU 视锥剔除：把 viewProj 的 6 个裁剪平面提取出来，逐个测试实例的包围球。
//
// 注意这是 **CPU 侧**的可见性判断，不是 GPU Culling —— M7 只用它来产生 "Visible Count"
// 这个统计量（以及在按键切换时对比「全部提交」与「只提交可见」的 CPU 成本差异）。
// GPU 端的剔除是 M9 之后的事。
//
// outVisibleIndices 按实例原始顺序填充，因此提交顺序仍然是确定性的。
void CullInstancesByFrustum(const std::vector<InstanceData>& instances,
                            DirectX::FXMMATRIX viewProj,
                            std::vector<std::uint32_t>& outVisibleIndices);
