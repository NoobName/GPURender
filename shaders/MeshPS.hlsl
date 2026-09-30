// MeshPS.hlsl - 像素着色器：采样基础颜色纹理 + 简单 Lambert 光照。
//
// 两个关键绑定对象：
//   t0 = SRV（Texture2D）  ：由 Root Signature 的 Descriptor Table 绑定，
//                            绘制前用 SetGraphicsRootDescriptorTable 指向具体描述符槽位。
//   s0 = Sampler（SamplerState）：这里是 Root Signature 里的「静态采样器」，
//                            在 PSO/Root Signature 创建时就固定下来，不需要每次绑定。
// 采样器决定「怎么采样」：过滤方式（点 / 线性 / 各向异性）与寻址模式（wrap / clamp / mirror）。

Texture2D    baseColorTexture : register(t0);
SamplerState linearSampler    : register(s0);

cbuffer ObjectConstants : register(b0)
{
    float4x4 model;
    float4x4 viewProj;
    float4   lightDirection;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    // 纹理采样：UV 由顶点插值而来，Sampler 决定过滤与寻址。
    const float3 albedo = baseColorTexture.Sample(linearSampler, input.uv).rgb;

    // 简单 Lambert 光照：让球体的立体感可见，便于验证法线与 UV 是否接对。
    // 光照本身不是 M6 的重点，这里只为「看得出形状」。
    // lightDirection 的语义是「从表面指向光源」的方向（C++ 侧已归一化）。
    const float3 N = normalize(input.normal);
    const float  ndl = saturate(dot(N, lightDirection));
    const float3 lit = albedo * (0.30 + 0.70 * ndl);

    return float4(lit, 1.0);
}
