// GenerateDrawCommandsCS.hlsl - 为**每个 LOD** 生成一条 DrawIndexed 间接命令（M12 / M16）
//
// 这是「GPU-Driven Rendering」里把**可见性 + LOD 选择结果**翻译成**绘制命令**的一步：
// 它写 N 条命令（N = LOD 级数），每条命令带：
//   * 自己那一级的几何偏移（StartIndexLocation / BaseVertexLocation）—— 来自 LOD 元数据；
//   * 自己那一级的实例数（InstanceCount）—— 来自 GPU 侧的计数器。
//
// 于是「画多少个实例」和「用哪一级几何」**两个决定都留在 GPU 上**。
//
// -----------------------------------------------------------------------------
// 关于 StartInstanceLocation：一个被证伪的假设（务必先读，M12 的教训）
// -----------------------------------------------------------------------------
//   最初的想法是「每个可见实例生成一条命令，用 StartInstanceLocation
//   把实例 ID 传给顶点着色器」，期望 SV_InstanceID == StartInstanceLocation。
//
//   **实测证明这在 D3D12 里不成立。** 官方对 StartInstanceLocation 的定义是：
//
//       "A value added to each index before reading per-instance data
//        from a vertex buffer."
//
//   也就是说它只影响「从**顶点缓冲**读取每实例数据（D3D11 风格的
//   PER_INSTANCE_DATA）时的索引偏移」，**不会**进入 SV_InstanceID。
//   SV_InstanceID 在每次 draw 内部恒从 0 开始。
//
//   实测现象：1570 条命令、每条 InstanceCount=1、StartInstanceLocation 各不相同，
//   结果全部画成了实例 0（1570 个重叠在一起），且不报任何错。
//
// -----------------------------------------------------------------------------
// 现在的做法：每级一条命令 + 顶点着色器查表
// -----------------------------------------------------------------------------
//   第 lod 条命令：InstanceCount = 该级的实例数，
//   StartIndexLocation / BaseVertexLocation 指向该级的几何，
//   **StartInstanceLocation 恒为 0**。
//
//   顶点着色器拿到的 SV_InstanceID 是 0..instanceCount-1，用它去索引
//   「该级的压缩列表」（SRV 由 CPU 在每次 ExecuteIndirect 之前换段）。
//   这样顶点着色器**完全不需要知道 LOD 的存在** —— 与 M12 的行为一致。
//
// -----------------------------------------------------------------------------
// 命令布局
// -----------------------------------------------------------------------------
//   D3D12_DRAW_INDEXED_ARGUMENTS = 20 字节：
//       +0  IndexCountPerInstance
//       +4  InstanceCount
//       +8  StartIndexLocation
//       +12 BaseVertexLocation
//       +16 StartInstanceLocation
//   本 Shader 把第 lod 条写在 lod * 20 处。

static const uint kDrawIndexedArgumentSize = 20u;

struct MeshLODRange
{
    uint  indexOffset;
    uint  indexCount;
    int   baseVertex;
    uint  triangleCount;
    float screenSizeThreshold;
    uint  vertexCount;
    uint  pad0;
    uint  pad1;
};

StructuredBuffer<MeshLODRange> gLODRanges : register(t0);

// 每个 LOD 一个实例计数器（4 字节一项）
ByteAddressBuffer gLODCounts : register(t1);

RWByteAddressBuffer gIndirectArguments : register(u0);

cbuffer CommandConstants : register(b0)
{
    uint gLODCount;
    uint gPad0;
    uint gPad1;
    uint gPad2;
};

[numthreads(8, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint lod = dispatchThreadId.x;
    if (lod >= gLODCount)
    {
        return;
    }

    const MeshLODRange range = gLODRanges[lod];

    // 该级的实例数来自 GPU 的计数器。CPU 从不读它 —— 它只是 GPU 内部的一份数据。
    // 这就是「命令内容由 GPU 决定」的落点。
    const uint instanceCount = gLODCounts.Load(lod * 4u);

    const uint base = lod * kDrawIndexedArgumentSize;

    gIndirectArguments.Store(base + 0u,  range.indexCount);        // 该级的索引数
    gIndirectArguments.Store(base + 4u,  instanceCount);           // <- GPU 决定
    gIndirectArguments.Store(base + 8u,  range.indexOffset);       // 该级索引起点
    gIndirectArguments.Store(base + 12u, (uint)range.baseVertex);  // 该级顶点基址
    gIndirectArguments.Store(base + 16u, 0u);                      // 恒为 0，见文件顶部
}
