// InstanceValidationCS.hlsl - 验证 GPU 上的实例数据与 CPU 侧完全一致。
//
// 为什么需要这个 Compute Shader：
//   M9 把 InstanceData 直接按二进制写进 GPU Buffer，C++ 与 HLSL 共享同一份
//   布局约定。C++ 侧能靠 static_assert 把布局钉死，但 **HLSL 侧没有任何
//   编译期手段**可以断言「我认为的 sizeof(InstanceData) 是多少」。
//   一旦两侧布局不一致（典型错误：忘记把 88 字节补齐到 96），
//   从第二个实例开始所有字段都会整体错位，而且 D3D12 不会报任何错。
//
//   所以这里用运行期验证补上缺口：
//     1. 上报 HLSL 自己算出的 sizeof(InstanceData)，供 CPU 比对；
//     2. 逐实例做自洽性检查（世界位置 == 包围球球心、padding 必须为 0 等），
//        这类检查在布局错位时**必然失败**；
//     3. 把前若干个实例的原始 96 字节原样摊平写出去，
//        让 CPU 能做逐字节 memcmp —— 这是最强的等价性证明。
//
// 设计取舍：
//   * **每个实例写一个错误码**，而不是用 InterlockedAdd 累计计数。
//     这样既不需要事先清零缓冲区，也不会有原子操作开销，
//     CPU 读回后还能按 bit 精确定位「是哪一类检查失败了」。
//   * M9 不做任何剔除，这个 CS 只读实例数据、不修改它。

// -----------------------------------------------------------------------------
// 与 src/Scene/InstanceData.h **逐字段对应**的结构体定义。
// 任何一侧改动都必须同步另一侧（见该头文件的布局表）。
//
// **row_major 是这里的要点，也是最容易踩的坑：**
//
//   HLSL 的 float4x4 默认是 **column-major** —— 即把结构化缓冲里的 16 个 float
//   按「列」来解释。而 C++ 侧的 XMFLOAT4X4 是 row-major（XMStoreFloat4x4 按行写出）。
//   两者直接对接的话，HLSL 读到的矩阵是 C++ 矩阵的**转置**：
//     world[3]     在 C++ 里是平移行 (tx,ty,tz,1)，
//                  在 column-major 解读下却变成了第 3 列，也就是 (0,0,0,1) 恒值。
//
//   本项目在 cbuffer 路径（MeshVS.hlsl）里是**故意**利用这个转置的：
//   原样上传 + mul(matrix, vector) 正好等于 row-vector 的 v * M。
//   但结构化缓冲是「数据」而不是「绑定」，让字段含义随存储方向漂移太危险，
//   所以这里显式声明 row_major，让 HLSL 的 world[r][c] 与 C++ 的 m[r][c] 一一对应。
//
//   > 后续里程碑若要用这个矩阵做顶点变换，必须配套写 mul(vector, matrix)
//     （而不是 mul(matrix, vector)），否则会得到转置的结果。
// -----------------------------------------------------------------------------
struct InstanceData
{
    row_major float4x4 world; // offset  0 (64B) 与 C++ 的 XMFLOAT4X4 逐元素对应，行 3 是平移
    float4   boundingSphere;  // offset 64 (16B) xyz = 世界空间球心, w = 半径
    uint     meshIndex;       // offset 80 (4B)
    uint     materialIndex;   // offset 84 (4B)
    uint2    padding;         // offset 88 (8B) -> 结构体补齐到 96
};

// CPU 侧期望的布局常量，原样回传以便日志自解释
static const uint kCpuExpectedSize = 96u;

// 每个实例的原始字节摊平后占多少个 float4（96 / 16）
static const uint kFloatsPerInstance = 6u;

// 错误码 bit 定义（0 = 全部通过）
static const uint kErrorPositionMismatch = 1u;  // world 平移 != boundingSphere.xyz
static const uint kErrorRadiusInvalid    = 2u;  // 半径 <= 0
static const uint kErrorPaddingNotZero   = 4u;  // padding != 0（布局错位时最敏感）
static const uint kErrorIndicesInvalid   = 8u;  // mesh/material 索引不是 0
static const uint kErrorNotAffine        = 16u; // world 最后一列不是 (0,0,0,1)

// 世界位置与包围球球心是同一份数据的两种存放，允许浮点误差
static const float kPositionTolerance = 1e-4f;

StructuredBuffer<InstanceData> gInstances : register(t0);
RWStructuredBuffer<uint>       gResults   : register(u0); // 每实例一个错误码
RWStructuredBuffer<float4>     gDump      : register(u1); // [0] = info, [1..] = 原始字节

// 由 CPU 通过 32-bit root constants 传入
cbuffer ValidationParams : register(b0)
{
    uint gInstanceCount;     // 场景实例总数
    uint gDumpInstanceCount; // 需要 dump 原始字节的实例数
    uint gPad0;
    uint gPad2;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;

    if (index == 0u)
    {
        // 本 CS 存在的核心目的之一：让 HLSL 在运行期自报家门。
        // CPU 拿到这个值后与 sizeof(InstanceData) 比对，就能确认两侧布局一致。
        gDump[0] = float4(asfloat((uint)sizeof(InstanceData)),
                          asfloat(kCpuExpectedSize),
                          asfloat(gInstanceCount),
                          asfloat(gDumpInstanceCount));
    }

    if (index >= gInstanceCount)
    {
        return;
    }

    const InstanceData instance = gInstances[index];

    // -------------------------------------------------------------------------
    // 自洽性检查：布局一旦错位，这些检查必然失败。
    //
    // 因为错位会让 world 与 boundingSphere 取自不同实例的不同偏移，
    // 它们的数值关系（世界位置 == 球心）立刻被破坏。
    // -------------------------------------------------------------------------
    uint error = 0u;

    // 旋转与缩放都绕实例中心，因此 world 的平移分量（行 3 的 xyz）
    // 应当与包围球球心完全一致。
    const float3 translation =
        float3(instance.world[3][0], instance.world[3][1], instance.world[3][2]);
    if (distance(translation, instance.boundingSphere.xyz) >= kPositionTolerance)
    {
        error |= kErrorPositionMismatch;
    }

    // 半径必须为正（实例缩放恒大于 0）
    if (instance.boundingSphere.w <= 0.0f)
    {
        error |= kErrorRadiusInvalid;
    }

    // padding 在 CPU 侧被显式置 0。
    // **这是对步长最敏感的检查**：如果 HLSL 认为元素是 88 字节而 CPU 按 96 写，
    // 这里读到的就会是相邻实例的字段而不是 0。
    if (instance.padding.x != 0u || instance.padding.y != 0u)
    {
        error |= kErrorPaddingNotZero;
    }

    // M9 的场景所有实例共用同一个网格与材质
    if (instance.meshIndex != 0u || instance.materialIndex != 0u)
    {
        error |= kErrorIndicesInvalid;
    }

    // 仿射变换：最后一列必须是 (0,0,0,1)
    if (instance.world[0][3] != 0.0f || instance.world[1][3] != 0.0f ||
        instance.world[2][3] != 0.0f || instance.world[3][3] != 1.0f)
    {
        error |= kErrorNotAffine;
    }

    gResults[index] = error;

    // -------------------------------------------------------------------------
    // 原始字节 dump：前 gDumpInstanceCount 个实例按 96 字节原样摊平写出。
    // 每个实例恰好 6 个 float4，CPU 读回后可直接与自己的数据 memcmp。
    // -------------------------------------------------------------------------
    if (index < gDumpInstanceCount)
    {
        const uint base = 1u + index * kFloatsPerInstance; // [0] 被 info 占用
        gDump[base + 0u] = instance.world[0];
        gDump[base + 1u] = instance.world[1];
        gDump[base + 2u] = instance.world[2];
        gDump[base + 3u] = instance.world[3];
        gDump[base + 4u] = instance.boundingSphere;
        gDump[base + 5u] = float4(asfloat(instance.meshIndex),
                                  asfloat(instance.materialIndex),
                                  asfloat(instance.padding.x),
                                  asfloat(instance.padding.y));
    }
}
