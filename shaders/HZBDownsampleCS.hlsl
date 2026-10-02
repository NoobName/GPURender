// HZBDownsampleCS.hlsl - HZB 逐级降采样（M14）
//
// 从 HZB 的第 i 级生成第 i+1 级：读 2x2 块，Max 归约，写一个像素。
// 重复 dispatch 直到 1x1，就得到完整的深度金字塔。
//
// =============================================================================
// Reduction Convention（与 HZBInitCS 一致，这里再强调一次）
// =============================================================================
//   投影约定：传统 DirectX 深度，Near -> 0、Far -> 1，**深度越大越远**
//   归约算子：**Max**
//
//   保守性证明（对任意一级都成立）：
//     设第 i 级某个像素的代表值 h = max(该像素覆盖的所有第 0 级深度)
//     那么 h 同时也是「该像素覆盖的屏幕区域」内深度的**最大值**。
//
//     若物体的最近深度 z_obj > h，
//     则 z_obj 大于该区域内**所有**像素的深度，
//     即物体被这一整块屏幕区域完全遮挡 —— 判定成立是**充分**的。
//
//     归纳地：第 i+1 级 = max(第 i 级的 2x2)，
//     所以每一级都精确保持「覆盖区域内深度的最大值」这个不变量 ✓
//
//   > 这正是 HZB 能用来做大物体快速遮挡查询的根据：
//   > 一个跨越 NxN 像素的物体，只需要查**一个**HZB 像素就能保守判定，
//   > 不必逐个测试它覆盖的所有深度。详见 LEARNING_NOTES M14 Q3。
//
// =============================================================================
// 为什么用 UAV 读而不是 SRV 读
// =============================================================================
//   整个 HZB 纹理在构建期间保持在 UNORDERED_ACCESS 状态 ——
//   因为每一级的输出就是下一级的输入，如果每级之间都做
//   UNORDERED_ACCESS <-> NON_PIXEL_SHADER_RESOURCE 的状态转换，会产生大量
//   barrier；而 D3D12 不允许同一资源同时处于「SRV 可读」与「UAV 可写」两个状态。
//
//   RWTexture2D<float> 的 Load 读是合法的，所以这里用 UAV 读源、UAV 写目标，
//   级间只需要一个 **UAV barrier**（保证上一级的写入对下一级的读取可见）。
//   —— 注意 UAV barrier 管的是「两次 UAV 访问的顺序」，
//      与状态转换是两件事（M10/M11 已经反复强调过）。
//
//   代价是失去只读缓存的优化 —— 对这么小的数据量可以忽略，
//   换来的是更少的 barrier 与更简单的状态管理。

RWTexture2D<float> gSrcMip : register(u0);
RWTexture2D<float> gDstMip : register(u1);

cbuffer HZBConstants : register(b0)
{
    uint2 gSrcSize;      // 源 mip 尺寸
    uint2 gDstSize;      // 目标 mip 尺寸 = ceil(src / 2)
    uint  gSrcMipLevel;  // 源 mip 索引（仅用于调试输出）
    uint  gPad0;
    uint  gPad1;
    uint  gPad2;
};

// 见 HZBInitCS.hlsl 的详细说明：夹取在奇数尺寸下只会重复块内自己的像素，
// 因此 Max 的结果仍然精确等于该块（边界处退化为更小区域）的最大深度。
float LoadClamped(RWTexture2D<float> tex, uint2 coord, uint2 size)
{
    return tex[min(coord, size - 1u)];
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 dst = dispatchThreadId.xy;
    if (dst.x >= gDstSize.x || dst.y >= gDstSize.y)
    {
        return;
    }

    const uint2 src = dst * 2u;

    const float d0 = LoadClamped(gSrcMip, src + uint2(0u, 0u), gSrcSize);
    const float d1 = LoadClamped(gSrcMip, src + uint2(1u, 0u), gSrcSize);
    const float d2 = LoadClamped(gSrcMip, src + uint2(0u, 1u), gSrcSize);
    const float d3 = LoadClamped(gSrcMip, src + uint2(1u, 1u), gSrcSize);

    // Max 归约：保证「每级都等于其覆盖区域内深度的最大值」
    gDstMip[dst] = max(max(d0, d1), max(d2, d3));
}
