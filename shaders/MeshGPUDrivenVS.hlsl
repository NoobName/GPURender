// MeshGPUDrivenVS.hlsl - GPU-Driven 路径的顶点着色器（M12）
//
// 与 MeshVS.hlsl（CPU-Driven 路径）的区别：
//
//   CPU-Driven：CPU 逐个实例 SetGraphicsRootConstantBufferView 绑定一份
//               ObjectConstants（每实例一个 256 字节槽位），然后 DrawIndexedInstanced。
//
//   GPU-Driven：实例数据**常驻显存**（M9 建立的 StructuredBuffer），
//               顶点着色器用 SV_InstanceID 自己查表取，CPU 只提交一次
//               ExecuteIndirect（而且永远只有一条命令）。
//
// -----------------------------------------------------------------------------
// 数据流：SV_InstanceID -> 压缩列表 -> 实例缓冲
// -----------------------------------------------------------------------------
//   间接命令里的 InstanceCount = GPU 算出的可见数，因此本次 draw 的
//   SV_InstanceID 取值是 0 .. visibleCount-1 —— 它是「第几个**可见**实例」，
//   **不是**真正的实例 ID。
//
//   真正的实例 ID 存在 M11 压缩出来的列表里：
//
//       realInstanceId = gVisibleInstanceIndices[SV_InstanceID]
//
//   这一次额外的间接寻址换来的是「可见实例在显存里连续」——
//   顶点着色器读到的实例数据是连续的，缓存局部性比按原始 ID 稀疏访问好得多。
//
//   > 为什么不用 StartInstanceLocation 直接传实例 ID：
//   > 实测 D3D12 的 SV_InstanceID 不受它影响（详见 GenerateDrawCommandsCS.hlsl
//   > 顶部的说明）。那是一条死路，且不报任何错。
//
// -----------------------------------------------------------------------------
// 矩阵约定：同一个着色器里两种约定共存
// -----------------------------------------------------------------------------
//   world（来自 StructuredBuffer，显式 row_major）
//       HLSL 读到的就是 C++ 的 XMFLOAT4X4（行主序，未转置），
//       语义是 row-vector 的 v' = v * M，所以要写 mul(vector, matrix)。
//
//   gViewProj（来自 cbuffer，默认 column-major）
//       HLSL 读到的是 C++ 矩阵的**转置**，转置之后正好等价于 v * M_cpp，
//       所以要写 mul(matrix, vector)。
//
//   两者写反的后果：投影矩阵被转置使用，所有顶点被推到屏幕外，
//   画面**完全空白**且没有任何报错。

struct InstanceData
{
    row_major float4x4 world; // 与 C++ 的 XMFLOAT4X4 一致，行 3 是平移
    float4   boundingSphere;  // xyz = 世界空间球心, w = 半径
    uint     meshIndex;       // 引用场景网格数组
    uint     materialIndex;   // 引用场景材质数组
    uint2    padding;         // 补齐到 96 字节
};

// t0 = 实例数据（常驻显存），t1 = 压缩后的可见实例索引（M11 产出）
//
// 可见索引的 SRV 是 **structured** 视图（StructureByteStride = 4），
// 因此 HLSL 侧必须用 StructuredBuffer<uint> 声明。
// 用 ByteAddressBuffer 声明会与视图类型不匹配（那要求 stride = 0 的 RAW 视图）。
StructuredBuffer<InstanceData> gInstances             : register(t0);
StructuredBuffer<uint>         gVisibleInstanceIndices : register(t1);

// 全局常量（每帧一份，而不是每实例一份）
cbuffer GlobalConstants : register(b0)
{
    float4x4 gViewProj;
    float4   gLightDirection;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
};

VSOutput main(VSInput input, uint visibleIndex : SV_InstanceID)
{
    // SV_InstanceID 是「第几个可见实例」，查压缩列表换算成真正的实例 ID
    const uint instanceId = gVisibleInstanceIndices[visibleIndex];
    const float4x4 world = gInstances[instanceId].world;

    // 局部 -> 世界：row_major 的 world，用 mul(vector, matrix)
    const float4 worldPosition = mul(float4(input.position, 1.0f), world);
    const float3 worldNormal = normalize(mul(float4(input.normal, 0.0f), world).xyz);

    VSOutput output;
    // 世界 -> 裁剪：cbuffer 的 gViewProj（已被 HLSL 读成转置），用 mul(matrix, vector)
    output.position = mul(gViewProj, worldPosition);
    output.normal = worldNormal;
    output.uv = input.uv;
    return output;
}
