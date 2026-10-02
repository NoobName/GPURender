// HZBVisualizePS.hlsl - 可视化 HZB 的任意一级 Mip（M14）
//
// 把选定的 mip 放大铺满屏幕，用灰度显示其 Max 深度值。
// 这是验证「每一级的归约是否正确」最直接的手段：
//   * Mip 0 应当与线性化后的深度图轮廓一致；
//   * 每往上一级，块状感越强、越亮（因为 Max 取的是块里最远的值）；
//   * 最后一级是 1x1，整屏只有一个颜色 —— 它等于**全屏最远**的深度。
//
// 用 SampleLevel 显式指定 mip 与线性过滤，这样放大时是平滑插值而不是
// 最近邻的硬块 —— 便于观察同一区域的深度分布；
// 若想看清每个金字塔像素的边界，把过滤改成 point 即可。

Texture2D<float> gHZBTexture : register(t0);
SamplerState     gHZBSampler : register(s0);

cbuffer HZBVisualizeConstants : register(b0)
{
    uint gMipLevel;      // 要查看的 mip 索引
    uint gMipCount;      // 总级数（用于归一化灰度）
    float gNearPlane;
    float gFarPlane;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD;
};

float4 main(PSInput input) : SV_TARGET
{
    // SampleLevel 显式指定 mip —— 不能用 Sample（那会按 uv 导数自动选 mip，
    // 结果看到的永远是接近全分辨率的那一级，达不到「查看指定 mip」的目的）。
    const float hzbDepth = gHZBTexture.SampleLevel(gHZBSampler, input.uv, (float)gMipLevel);

    // 1.0 表示「该区域没有被任何几何覆盖」——背景。
    // 用深蓝标出来，与深度可视化保持一致。
    if (hzbDepth >= 1.0f)
    {
        return float4(0.02f, 0.02f, 0.05f, 1.0f);
    }

    // 线性化：HZB 里存的是投影后的非线性 NDC 深度（NDC z ∈ [0,1]），
    // 直接显示会让绝大部分值挤在接近 1 的白色区域。
    // 反解出视空间距离才有可读性（推导见 DepthVisualizePS.hlsl）。
    const float z = (gNearPlane * gFarPlane) /
                    (gFarPlane - hzbDepth * (gFarPlane - gNearPlane));

    const float displayRange = gFarPlane * 0.25f;
    const float t = saturate(1.0f - (z - gNearPlane) / displayRange);

    // 越高的 mip（越小的分辨率）用偏暖的色调，便于一眼看出当前在哪一级：
    //   mip 0 -> 中性灰    mip 越大 -> 越偏黄绿
    const float mipRatio = (gMipCount > 1u) ? (float)gMipLevel / (float)(gMipCount - 1u) : 0.0f;
    const float3 tint = lerp(float3(1.0f, 1.0f, 1.0f), float3(0.75f, 1.0f, 0.45f), mipRatio);

    return float4(t * tint, 1.0f);
}
