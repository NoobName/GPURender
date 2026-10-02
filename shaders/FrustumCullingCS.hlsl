// FrustumCullingCS.hlsl - GPU 视锥剔除 + **LOD 选择** + Stream Compaction（M11 / M16）
//
// 一个线程处理一个实例，依次做三件事：
//   ① 6 平面保守视锥测试；
//   ② 通过测试后，按**投影后的屏幕尺寸**选一个 LOD 级；
//   ③ 用一次原子加拿到「该 LOD 段内的输出下标」，把实例 ID 写进对应分段。
//
// -----------------------------------------------------------------------------
// 为什么 LOD 选择放在这里，而不是单独开一个 Pass（M16）
// -----------------------------------------------------------------------------
//   投影尺寸的计算需要 viewProj 与包围球 —— 这两样在视锥测试里**已经算过**。
//   单独开一个 Pass 意味着把实例表再遍历一遍、矩阵再乘一遍，
//   收益只是"代码分成两个文件"。合并之后：
//
//     * 一次遍历同时产出「可见性」与「LOD」两个结果；
//     * LOD 分段与视锥压缩共用同一次原子操作 —— 压缩代价没有翻倍。
//
// -----------------------------------------------------------------------------
// 为什么输出要**按 LOD 分段**（M16 的核心）
// -----------------------------------------------------------------------------
//   间接绘制命令里能表达的几何选择只有三个字段：
//       IndexCountPerInstance / StartIndexLocation / BaseVertexLocation
//   它们是**每条命令**一份的。所以「让不同实例用不同 LOD」的唯一办法是
//   **每个 LOD 一条命令**，每条带自己的几何偏移、自己那批实例。
//
//   于是输出布局变成：
//
//       gLODInstanceIndices[ lod * gSegmentCapacity + slot ] = instanceIndex
//       gLODCounts[ lod * 4 ]                                = 该级实例数
//
//   命令生成 CS 随后为每个 lod 写一条命令，InstanceCount 取自 gLODCounts[lod]。
//   CPU 全程不参与，也不知道任何一级有多少实例。
//
// -----------------------------------------------------------------------------
// LOD 判据：投影后的屏幕尺寸
// -----------------------------------------------------------------------------
//   包围球中心投影到 NDC，半径按透视缩放换算，得到**该球投影直径占屏幕高度的比例**：
//
//       screenSize = 2 * r * P11 / sqrt(d^2 - r^2)
//
//   分母里的 sqrt 项是球到切平面的距离（M15 已推导）——
//   保留它才能得到**包住整个球**的投影半径，与遮挡剔除的估计保持一致。
//
//   为什么不用世界空间距离：见 LEARNING_NOTES M16 Q1。
//   要点是「视觉重要性 = 屏幕占比」，而屏幕占比同时取决于距离、FOV、
//   分辨率与物体自身大小 —— 距离只是其中一个因子。
//
// -----------------------------------------------------------------------------
// 与 src/Scene/InstanceData.h 逐字段对应（row_major 的约定见 M9）。
// -----------------------------------------------------------------------------
struct InstanceData
{
    row_major float4x4 world; // offset  0 (64B)
    float4   boundingSphere;  // offset 64 (16B) xyz = 世界空间球心, w = 半径
    uint     meshIndex;       // offset 80 (4B)
    uint     materialIndex;   // offset 84 (4B)
    uint2    padding;         // offset 88 (8B) -> 结构体补齐到 96
};

// LOD 元数据（与 C++ 的 MeshLODRange 逐字段对应，stride = 32）
struct MeshLODRange
{
    uint  indexOffset;          // 合并索引缓冲里的起始索引
    uint  indexCount;
    int   baseVertex;
    uint  triangleCount;
    float screenSizeThreshold;  // 屏幕尺寸低于此值 -> 切到下一级
    uint  vertexCount;
    uint  pad0;
    uint  pad1;
};

StructuredBuffer<InstanceData> gInstances : register(t0);
StructuredBuffer<MeshLODRange> gLODRanges : register(t2);

// 按 LOD 分段的压缩列表。第 lod 段从 lod * gSegmentCapacity 开始。
RWStructuredBuffer<uint> gLODInstanceIndices : register(u0);

// 每个 LOD 一个计数器：gLODCounts[lod * 4] = 该级实例数
RWByteAddressBuffer gLODCounts : register(u1);

// 每实例的 LOD（**仅用于可视化**）。
//
// 剔除逻辑本身不需要它 —— 但它让调试视图能把每个实例按所选 LOD 着色，
// 从而一眼看出「投影尺寸判据是否按预期切换」。这是验证 LOD 最直观的手段。
RWStructuredBuffer<uint> gInstanceLOD : register(u2);

cbuffer FrustumConstants : register(b0)
{
    float4 gFrustumPlanes[6]; // (nx, ny, nz, d)，法线已归一化
    float4x4 gViewProj;       // LOD 投影尺寸需要（cbuffer 默认 column-major -> mul(M, v)）

    uint  gInstanceCount;
    uint  gLODCount;
    uint  gSegmentCapacity;
    uint  gPad0;

    float gProj11;   // 投影矩阵 [1][1]（y 缩放）
    float gLODBias;  // 全局 LOD 偏置（正数 = 更偏向高精度）
    uint  gPad1;
    uint  gPad2;
};

// 按屏幕尺寸挑 LOD。
//
// 语义：threshold[i] 是「继续用第 i 级」的下界，低于它就用第 i+1 级。
// 从前往后扫、满足条件就往后推进，因此即使阈值数组不是单调的也不会越界。
uint SelectLOD(float screenSize)
{
    uint lod = 0u;
    for (uint i = 0u; i + 1u < gLODCount; ++i)
    {
        if (screenSize < gLODRanges[i].screenSizeThreshold)
        {
            lod = i + 1u;
        }
    }
    return lod;
}

void EmitInstance(uint lod, uint instanceIndex)
{
    uint outputIndex = 0u;
    // 每级的计数器在自己的 4 字节槽位上，所以不同 LOD 之间**没有原子竞争**
    gLODCounts.InterlockedAdd(lod * 4u, 1u, outputIndex);
    gLODInstanceIndices[lod * gSegmentCapacity + outputIndex] = instanceIndex;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;

    // 线程组数向上取整，最后一组通常有富余线程，必须挡掉。
    if (index >= gInstanceCount)
    {
        return;
    }

    const InstanceData instance = gInstances[index];
    const float4 sphere = instance.boundingSphere;
    const float radius = sphere.w;

    // ---- ① 保守视锥测试：只有球**完全**落在某个平面外侧才剔除 ----
    bool visible = true;
    [unroll]
    for (uint planeIndex = 0u; planeIndex < 6u; ++planeIndex)
    {
        const float4 plane = gFrustumPlanes[planeIndex];
        const float distance = plane.x * sphere.x + plane.y * sphere.y +
                               plane.z * sphere.z + plane.w;
        if (distance < -radius)
        {
            visible = false;
            break;
        }
    }

    if (!visible)
    {
        return; // 被剔除的实例不占任何 LOD 段的槽位
    }

    // ---- ② 按投影尺寸选 LOD ----
    //
    // 包围球中心的裁剪空间坐标；w 对标准透视投影就是视空间深度。
    const float4 clip = mul(gViewProj, float4(sphere.xyz, 1.0));

    uint lod = 0u;
    if (clip.w > radius)
    {
        // 球到切平面的距离：用它作分母才能得到**包住整个球**的投影半径
        const float denom = sqrt(max(clip.w * clip.w - radius * radius, 1e-6));
        // NDC 的 y 半径 -> 「占屏幕高度比例」就是它的 2 倍
        const float screenSize = 2.0 * radius * gProj11 / denom;
        lod = SelectLOD(screenSize + gLODBias);
    }
    // 球包含相机时半径无定义 -> 用最高精度级（lod = 0），
    // 与遮挡剔除在这里「保守放行」的选择一致：宁可画贵的，不可画错的。

    // ---- ③ Stream Compaction 到该 LOD 的段 ----
    //
    //   InterlockedAdd(dest, value, originalValue)
    //     originalValue 是**加之前的旧值**，恰好就是「本线程是该级第几个实例」，
    //     可以直接当输出下标。原子性保证 N 次调用返回 0..N-1 的一个排列。
    EmitInstance(lod, index);

    // 顺带记录每个实例选了哪一级（**仅用于可视化**）。
    //
    // 这里按**实例下标**直接写，与压缩无关 —— 被剔除的实例保留上一帧的值，
    // 而它们本来就不参与绘制，所以不影响调试视图的正确性。
    // 它让调试视图能把每个实例按所选 LOD 着色，一眼看出尺寸判据是否生效。
    gInstanceLOD[index] = lod;
}
