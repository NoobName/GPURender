// DebugVS.hlsl - 调试线框顶点着色器（M8 的 Frustum / Bounding Sphere 可视化）
//
// 顶点**已经是世界空间坐标**（CPU 侧用 invViewProj 反算出来），
// 所以这里只需要 viewProj，不需要 model 矩阵。
//
// 刻意复用与网格相同的 ObjectConstants 布局与根签名：
// 这样调试渲染不需要额外的绑定、额外的描述符堆或额外的根签名，
// 换的只是 PSO（拓扑改成 LINELIST、关闭深度测试）。

cbuffer ObjectConstants : register(b0)
{
    float4x4 model;          // 线框不使用
    float4x4 viewProj;       // 世界 -> 裁剪
    float4   lightDirection; // 线框不使用
};

struct VSInput
{
    float3 position : POSITION; // 世界空间位置
    float4 color    : COLOR;    // 顶点颜色（逐顶点，让不同线框用不同颜色）
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = mul(viewProj, float4(input.position, 1.0));
    output.color = input.color;
    return output;
}
