// MeshVS.hlsl - 顶点着色器：模型空间 -> 世界空间 -> 裁剪空间，并传递法线与 UV。
//
// 矩阵约定（M5 踩过的坑，务必与 C++ 侧配套）：
//   C++ 侧 DirectXMath 是 row-major，矩阵**按原样**写入常量缓冲；
//   HLSL 的 float4x4 默认按 column-major 读取内存，于是读到的正好是原矩阵的转置，
//   配合 mul(matrix, vector) 即为正确的 row-vector 变换。
//   如果改成「上传前先转置」，这里就必须换成 mul(vector, matrix) —— 二者不可混用。

cbuffer ObjectConstants : register(b0)
{
    float4x4 model;          // 模型 -> 世界
    float4x4 viewProj;       // 世界 -> 裁剪
    float4   lightDirection; // 世界空间中「指向光源」的方向（C++ 侧已归一化）
};

struct VSInput
{
    float3 position : POSITION;  // 模型空间位置
    float3 normal   : NORMAL;    // 模型空间法线
    float2 uv       : TEXCOORD0; // 纹理坐标
};

struct VSOutput
{
    float4 position : SV_POSITION; // 裁剪空间位置（光栅化后做透视除法）
    float3 normal   : NORMAL;      // 世界空间法线（插值后由 PS 使用）
    float2 uv       : TEXCOORD0;   // 纹理坐标（插值后由 PS 采样）
};

VSOutput main(VSInput input)
{
    VSOutput output;

    const float4 worldPosition = mul(model, float4(input.position, 1.0));
    output.position = mul(viewProj, worldPosition);

    // 法线只关心方向，所以 w 分量传 0，从而忽略模型矩阵里的平移。
    // 严格做法是使用「世界矩阵的逆转置」，但本项目的 model 只含旋转与统一缩放，
    // 直接用 model 变换方向即可。
    output.normal = mul(model, float4(input.normal, 0.0)).xyz;

    output.uv = input.uv;
    return output;
}
