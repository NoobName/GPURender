// DebugPS.hlsl - 调试线框像素着色器
//
// 调试线框的唯一诉求是「看得清、能分辨」：
// 直接输出插值后的顶点颜色，不做光照也不做纹理采样。

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR;
};

float4 main(PSInput input) : SV_TARGET
{
    return input.color;
}
