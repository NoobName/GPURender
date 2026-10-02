// HZBInitCS.hlsl - 从全分辨率深度缓冲生成 HZB 的第 0 级（M14）
//
// =============================================================================
// Reduction Convention（本文件最重要的一行注释）
// =============================================================================
//   投影约定：**传统 DirectX 深度**，即 NDC z ∈ [0,1] 且
//                近平面 -> 0        远平面 -> 1
//             **深度值越大表示越远。**
//
//             （如果是 Reverse-Z 约定 —— 近 1 / 远 0 —— 下面的结论会**反过来**，
//               这一点必须和投影矩阵一起确认，不能凭印象。）
//
//   归约算子：**Max**（取 2x2 块里最大的深度值）
//
//   为什么必须是 Max（而不是 Min）—— 完整的数学推导：
//
//     设某个 2x2 块的深度集合为 D = { d0, d1, d2, d3 }，记
//         d_max = max(D)
//     设待测物体的**最近**深度为 z_obj。
//
//     遮挡剔除的安全性要求是「宁可多画，绝不能漏画」（conservative）：
//     只有当物体**确定被完全遮挡**时才允许剔除它。
//
//     ► 用 Max 作为该块的代表值：
//         若  z_obj > d_max
//         则  ∀i: z_obj > d_i        （因为 d_max 是上界）
//         即  物体比这一块里**每一个**像素都远
//         即  物体被这一块完全遮挡 —— 判定「被遮挡」是**充分**的 ✓
//
//     ► 若改用 Min 作为代表值：
//         条件变成 z_obj > d_min
//         这只说明物体比**最近的那个**像素远，
//         完全可能存在某个 d_j > z_obj（物体比它更近），
//         此时物体其实是可见的，却会被判定为遮挡 —— **误剔除** ✗
//
//     结论：在 Near=0 / Far=1 的约定下，**Max Reduction 是保守正确的**，
//           Min Reduction 会导致可见物体被错误剔除。
//
//     > 记忆方式：深度值越大越远 → 想要「最坏情况（最容易被遮挡的判据）」
//     > 就要取**最远**的值 → Max。
//     > Reverse-Z 下这个方向整体翻转 → 用 Min。
//
// =============================================================================
// 尺寸约定：为什么 HZB 的第 0 级是 (w/2, h/2) 而不是 (w, h)
// =============================================================================
//   全分辨率深度本身已经是精确值，把它原样拷进 HZB 并不增加信息量。
//   HZB 的语义是「每 2x2 区域的聚合深度」，所以第 0 级直接从
//   全分辨率深度降采样得到 —— 省下一级存储与一次 dispatch。
//
//   本 Shader 就是这一步：读全分辨率深度的 2x2 块，Max 归约，写 HZB[0]。

// 源：Depth Prepass 写出的全分辨率深度缓冲（D32_FLOAT 以 R32_FLOAT 的 SRV 暴露）
Texture2D<float> gDepthTexture : register(t0);

// 目标：HZB 的第 0 级（R32_FLOAT，UAV）
RWTexture2D<float> gHZBMip0 : register(u0);

cbuffer HZBConstants : register(b0)
{
    uint2 gSrcSize;        // 全分辨率深度尺寸
    uint2 gDstSize;        // HZB[0] 尺寸 = ceil(src / 2)
    uint  gSrcMipLevel;    // 恒为 0（深度缓冲没有 mip）
    uint  gPad0;
    uint  gPad1;
    uint  gPad2;
};

// 读取时把坐标夹到 [0, size-1] —— 这是处理**奇数尺寸**的关键。
//
// 为什么需要它：奇数尺寸下，2x2 的右下角会落在图像外面。
// 例如 W = 5 时，最后一个块的 src.x = 4，而 src.x + 1 = 5 已经越界。
//
// **夹取是严格安全的**，理由：
//   dst.x 的取值范围是 [0, ceil(W/2) - 1]，所以 src.x = 2 * dst.x 最大为
//       W - 1   (W 为奇数)   或   W - 2   (W 为偶数)
//   * W 为偶数时，src.x + 1 = W - 1，本来就在界内，夹取不生效；
//   * W 为奇数时，src.x + 1 = W 越界，夹取后得到 W - 1 ——
//     **这正是本块自己的左邻像素**（块范围是 [W-1, W] ∩ [0, W-1] = {W-1}），
//     并没有跨到相邻块去。
//
// 也就是说：夹取只会把「块内已有的像素」重复采样一次，
// 既不会引入块外的新值，也不会改变 Max 的结果 —— HZB 的每个值仍然
// 精确等于该 2x2 区域（在边界处退化为更小区域）内深度的最大值。
float LoadClamped(Texture2D<float> tex, uint2 coord, uint2 size)
{
    const uint2 clamped = min(coord, size - 1u);
    return tex.Load(int3(clamped, 0));
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

    const float d0 = LoadClamped(gDepthTexture, src + uint2(0u, 0u), gSrcSize);
    const float d1 = LoadClamped(gDepthTexture, src + uint2(1u, 0u), gSrcSize);
    const float d2 = LoadClamped(gDepthTexture, src + uint2(0u, 1u), gSrcSize);
    const float d3 = LoadClamped(gDepthTexture, src + uint2(1u, 1u), gSrcSize);

    // Max 归约：见文件顶部关于 Reduction Convention 的推导
    gHZBMip0[dst] = max(max(d0, d1), max(d2, d3));
}
