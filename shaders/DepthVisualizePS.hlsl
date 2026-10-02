// DepthVisualizePS.hlsl - 把深度缓冲画成灰度图（M13）
//
// 深度缓冲是 D32_FLOAT，这里以 R32_FLOAT 的 SRV 读取。
// 注意它存的是**投影后的非线性深度**（NDC z，D3D 下范围 [0,1]），
// 不是「到相机的距离」—— 近处的精度高、远处被压缩得很厉害，
// 所以直接显示 raw depth 时大部分物体都会挤在接近 1 的白色区域。
//
// 为了让可视化真正有信息量，这里做一次**线性化**，把 NDC 深度换算回
// 视空间距离，再按一个可调范围映射到灰度。

Texture2D<float> gDepthTexture : register(t0);
SamplerState     gDepthSampler : register(s0);

cbuffer DepthVisualizeConstants : register(b0)
{
    float2 gInvResolution;  // 1/宽, 1/高（本 shader 未用，保留给将来的邻域滤波）
    float  gNearPlane;      // 相机近平面
    float  gFarPlane;       // 相机远平面
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD;
};

float4 main(PSInput input) : SV_TARGET
{
    const float rawDepth = gDepthTexture.Sample(gDepthSampler, input.uv);

    // 深度缓冲里等于 1.0 的位置表示「没有几何覆盖」（清屏值），
    // 用深蓝标出来，一眼就能区分「背景」与「极远处的几何」。
    if (rawDepth >= 1.0f)
    {
        return float4(0.02f, 0.02f, 0.05f, 1.0f);
    }

    // 线性化：D3D 的透视投影下，NDC 深度 d 与视空间距离 z 的关系是
    //     d = (far * (z - near)) / (z * (far - near))
    // 反解得到：
    //     z = (near * far) / (far - d * (far - near))
    const float z = (gNearPlane * gFarPlane) /
                    (gFarPlane - rawDepth * (gFarPlane - gNearPlane));

    // 按「近处为白、远处为黑」映射，范围截到 [near, far] 的前 1/4 ——
    // 场景只有几十个单位深，用一个固定范围比按 far 归一化看得清楚得多。
    const float displayRange = gFarPlane * 0.25f;
    const float t = saturate(1.0f - (z - gNearPlane) / displayRange);

    return float4(t, t, t, 1.0f);
}
