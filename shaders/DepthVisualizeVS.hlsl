// DepthVisualizeVS.hlsl - 深度可视化用的全屏三角形（M13）
//
// 不需要顶点缓冲：用 SV_VertexID 现场生成三个覆盖全屏的顶点。
// 这是「全屏三角形」的标准写法 —— 比两个三角形的全屏 quad 少一次
// 对角线上的像素重复着色（GPU 以 2x2 quad 为光栅化单位，quad 的对角线
// 会让边界像素被处理两次）。
//
// 顶点位置与 UV：
//   vertexId = 0 -> position (-1,  1), uv (0, 0)   左上
//   vertexId = 1 -> position (-1, -3), uv (0, 2)   下方屏外（超采样覆盖左下）
//   vertexId = 2 -> position ( 3,  1), uv (2, 0)   右方屏外（超采样覆盖右上）
//
// 三个顶点都在裁剪空间内（w=1），光栅化后被视口裁剪成正好覆盖屏幕的三角形。

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    VSOutput output;

    // uv 取 0 / 2 / 2 的组合，覆盖 [0,2]x[0,2] 的区域
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);

    // 把 uv 从 [0,2] 映射到 NDC 的 [-1,3]（y 翻转，屏幕坐标向下）
    output.position = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);

    return output;
}
