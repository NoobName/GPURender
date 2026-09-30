// MeshGPUDrivenPS.hlsl - GPU-Driven 路径的像素着色器（M12）
//
// 与 MeshPS.hlsl（CPU-Driven 路径）的区别只在**数据来源**：
//
//   CPU-Driven：光照方向从每实例的 ObjectConstants（b0）读，材质纹理在 t0。
//   GPU-Driven：光照方向来自每帧一份的 GlobalConstants（b0），
//               材质纹理在 t1 —— 因为 t0/t1 被实例缓冲与可见索引列表占了（见 VS）。

cbuffer GlobalConstants : register(b0)
{
    float4x4 gViewProj;
    float4   gLightDirection;
};

Texture2D    gAlbedoTexture : register(t2);
SamplerState gAlbedoSampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
};

float4 main(PSInput input) : SV_TARGET
{
    const float3 albedo = gAlbedoTexture.Sample(gAlbedoSampler, input.uv).rgb;

    // Lambert 漫反射 + 环境项。
    //
    // 公式与 MeshPS.hlsl **逐字一致**，这样 GPU-Driven 与 CPU-Driven 两个模式的
    // 画面才能直接对比（曾因为这里把光照方向取反，导致两种模式有 83% 的像素不同 ——
    // 光照影响所有表面，所以一点点符号差异都会被放大到全屏）。
    // lightDirection 的语义是「从表面指向光源」的方向（C++ 侧已归一化）。
    const float3 N = normalize(input.normal);
    const float  ndl = saturate(dot(N, gLightDirection.xyz));
    const float3 lit = albedo * (0.30f + 0.70f * ndl);

    return float4(lit, 1.0f);
}
