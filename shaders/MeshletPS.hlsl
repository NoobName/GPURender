// MeshletPS.hlsl - Mesh Shader 路径的像素着色器（M18）
//
// =============================================================================
// 为什么需要一个专门的 PS，而不复用 MeshPS.hlsl
// =============================================================================
//   MeshPS.hlsl 的绑定契约是：
//       b0 = ObjectConstants{ model, viewProj, lightDirection }
//       t0 = baseColorTexture, s0 = static sampler
//
//   而 Mesh Shader 路径的根签名是：
//       b0 = MeshShaderConstants{ worldViewProj, color, meshletCount }
//       t0..t4 = meshlet 数据（meshlet / 局部顶点索引 / 打包三角形 / 剔除数据 / 顶点）
//
//   两者的 b0 布局与 t0 语义**完全不同**。PSO 里 MS 与 PS 必须共享同一套根绑定，
//   所以复用 MeshPS 会导致 t0 被同时当成「纹理」和「meshlet 数组」——
//   这正是那种「能编译、能跑、画面错得莫名其妙」的 bug。
//
//   因此这里写一个与 MS 严格共用 b0 的 PS。它刻意保持极简：
//   只做 Lambert 光照 + 常量色 —— 目的是让「几何是否正确」**一眼可判**，
//   而不是把注意力分散在材质上。
//
// =============================================================================
// 输入结构必须与 MeshletMS.hlsl 的 VertexOut 逐字段一致
// =============================================================================
//   Mesh Shader 的输出经光栅化插值后直接进入 PS，
//   语义名与类型必须匹配（SV_Position / NORMAL / TEXCOORD0）。

struct PSInput
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
};

// 与 MeshletMS.hlsl 的 cbuffer **必须完全一致** ——
// 它们绑定到同一个 b0（root constants），布局不同会造成读取错位。
cbuffer MeshShaderConstants : register(b0)
{
    float4x4 gWorldViewProj;
    float4   gColor;
    float4   gFrustumPlanes[6];
    float4   gEyePosition;
    uint     gMeshletCount;
    uint     gConeCullingEnabled;
    uint     gFrustumCullingEnabled;
    uint     gPad0;
};

float4 main(PSInput input) : SV_TARGET
{
    const float3 n = normalize(input.normal);

    // 固定方向光（世界空间）。不引入光源常量，让 Mesh Shader 路径的根签名保持最小。
    const float3 lightDir = normalize(float3(0.4f, 0.8f, -0.5f));

    // 单面 Lambert 光照；球体绕序已与外向法线对齐。
    const float ndotl = saturate(dot(n, lightDir));

    const float3 albedo = gColor.rgb;
    const float3 lit    = albedo * (0.25f + 0.75f * ndotl);

    return float4(lit, 1.0f);
}
