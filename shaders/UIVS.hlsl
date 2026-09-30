// UIVS.hlsl - 屏幕空间 UI 顶点着色器（M7 的实时统计文字）
//
// 与 MeshVS 的区别：
//   - 顶点只有 position(float2 像素坐标) + uv(float2)，没有法线/世界坐标概念
//   - 这里的 "viewProj" 实际是**正交投影**矩阵，把像素坐标直接映射到 NDC
//   - model 矩阵不使用（顶点已经是屏幕空间坐标）
//
// 矩阵约定与 MeshVS 保持一致：C++ 侧按原样上传，HLSL 按 column-major 读到的
// 正好是转置，配合 mul(matrix, vector) 即为正确的 row-vector 变换。

cbuffer ObjectConstants : register(b0)
{
    float4x4 model;          // UI 不使用
    float4x4 viewProj;       // 这里是正交投影矩阵
    float4   lightDirection; // UI 不使用
};

struct VSInput
{
    float2 position : POSITION;  // 像素坐标（左上为原点，y 向下）
    float2 uv       : TEXCOORD0; // 字体图集 UV
    float4 color    : COLOR;     // 文字颜色（用于画阴影 / 前景两遍）
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = mul(viewProj, float4(input.position, 0.0, 1.0));
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
