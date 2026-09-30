// UIPS.hlsl - 屏幕空间 UI 像素着色器
//
// 字体图集是「白色字形 + alpha 通道」，所以这里直接把 alpha 作为输出透明度，
// 交给 PSO 的 SrcAlpha / InvSrcAlpha 混合，就能得到抗锯齿的文字。

Texture2D    fontAtlas     : register(t0);
SamplerState linearSampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR;
};

float4 main(PSInput input) : SV_TARGET
{
    // 字形覆盖率来自图集的 alpha 通道，最终透明度 = 字形覆盖率 * 顶点颜色透明度。
    // 顶点颜色让同一个字符串可以画成白色前景 + 深色阴影两遍，保证任何背景下都清晰。
    const float coverage = fontAtlas.Sample(linearSampler, input.uv).a;
    const float alpha = coverage * input.color.a;
    if (alpha <= 0.0)
    {
        discard; // 完全透明的像素直接丢弃，省掉一次混合
    }
    return float4(input.color.rgb, alpha);
}
