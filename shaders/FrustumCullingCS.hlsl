// FrustumCullingCS.hlsl - GPU 视锥剔除 + Stream Compaction（M11）
//
// 一个线程处理一个实例：对它的世界空间包围球做 6 平面保守测试；
// 若可见，就用一次原子加拿到「输出下标」，把实例 ID 写进紧凑列表。
//
// -----------------------------------------------------------------------------
// 与 M10 的区别：不再写完整的布尔数组，而是写压缩后的索引列表
// -----------------------------------------------------------------------------
//   M10（未压缩）：                    M11（Stream Compaction）：
//     visibilityFlags[N]                 visibleInstanceIndices[visibleCount]
//     0 0 1 0 1 1 0 1                    2 4 5 7
//     （N 个 uint）                      （visibleCount 个 uint）
//
//   代价：需要一次原子操作 + 输出顺序不确定（取决于线程完成顺序）
//   收益：内存占用与下游读取量都正比于**可见数**而不是总数；
//         下游 Pass（剔除、LOD、间接绘制）可以直接顺序遍历这个列表，
//         不需要再跳过被剔除的槽位。
//
// 为什么这个任务适合 Compute Shader：
//   剔除本身是「尴尬并行」的逐元素判定（M10 已论证）；
//   压缩则更进一步 —— 它需要**跨线程协作**才能得到连续的输出下标，
//   而原子操作 / 前缀和正是 GPU 上做这件事的两种手段。
//   图形管线里没有别的阶段能既做批量判定、又做跨线程数据重排。
//
// -----------------------------------------------------------------------------
// 本版用 Atomic（InterlockedAdd）
// -----------------------------------------------------------------------------
//   每个可见实例调用一次 InterlockedAdd，拿到的返回值就是它独占的输出槽位。
//   正确性依赖「原子加返回旧值」这个语义：N 次调用必然返回 0..N-1 的一个排列，
//   因此不会有空洞、也不会互相覆盖。
//
//   瓶颈在于 **Atomic Contention**：所有线程争抢同一个地址。
//   详见 LEARNING_NOTES M11 —— 这也是未来改用 Scan-Based Compaction 的动机。

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

StructuredBuffer<InstanceData> gInstances : register(t0);

// 压缩后的可见实例 ID 列表。第 i 个可见实例的 ID 位于 [i]。
// 容量按最大实例数分配（最坏情况是全可见），实际有效长度由 gVisibleCount 给出。
RWStructuredBuffer<uint> gVisibleInstanceIndices : register(u0);

// 可见实例计数器。用 ByteAddressBuffer 而不是 StructuredBuffer<uint>：
//   * 它是一个 4 字节的「裸」缓冲，后续 ExecuteIndirect 的计数参数可以
//     直接指向同一块内存（M12 会用到这一点）；
//   * InterlockedAdd 在 ByteAddressBuffer 上以字节偏移寻址，语义更贴近硬件。
RWByteAddressBuffer gVisibleCount : register(u1);

// 视锥平面由 CPU 提取并归一化后经 root constants 传入（与 M10 一致）。
cbuffer FrustumConstants : register(b0)
{
    float4 gFrustumPlanes[6]; // (nx, ny, nz, d)，法线已归一化
    uint   gInstanceCount;
    uint   gPad0;
    uint   gPad1;
    uint   gPad2;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;

    // 线程组数向上取整，最后一组通常有富余线程，必须挡掉。
    if (index >= gInstanceCount)
    {
        return;
    }

    const float4 sphere = gInstances[index].boundingSphere;
    const float radius = sphere.w;

    // 保守测试：只有球完全落在某个平面外侧才判为剔除。
    // 算术写法与 CPU 侧逐字对应，便于两侧结果比对（见 M10）。
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
        return; // 被剔除的实例不占输出槽位 —— 这正是压缩的意义
    }

    // ---- Stream Compaction 的核心三行 ----
    //
    //   InterlockedAdd(dest, value, originalValue)
    //     dest          : 目标地址（这里用字节偏移 0）
    //     value         : 要加的值（1）
    //     originalValue : **加之前的旧值**，由硬件原子地返回
    //
    //   旧值恰好就是「本线程是第几个可见实例」，因此可以直接当输出下标。
    //   原子性保证了 N 个线程拿到的旧值互不相同且构成 0..N-1，
    //   所以既不会覆盖、也不会留下空洞。
    uint outputIndex = 0u;
    gVisibleCount.InterlockedAdd(0u, 1u, outputIndex);

    gVisibleInstanceIndices[outputIndex] = index;
}
