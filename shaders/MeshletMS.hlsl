// MeshletMS.hlsl - Mesh Shader 几何生成（M18）
//
// =============================================================================
// 一个 Mesh Shader 线程组处理一个 Meshlet
// =============================================================================
// 这是本阶段明确记录并解释的 Mapping：**1 个线程组 <-> 1 个 meshlet**。
//
// 为什么这样映射（而不是「一个线程组处理多个 meshlet」或「一个线程一个三角形」）：
//
//   * 一个 meshlet 的规模是 128 顶点 / 128 三角形，正好匹配一个线程组。
//     numthreads(128,1,1) 之后，「第 i 个线程处理第 i 个顶点 / 第 i 个三角形」
//     这种最朴素的写法就是天然负载均衡的 —— 不需要任何线程内循环。
//
//   * meshlet 是**独立剔除单位**，将来接入 Amplification Shader 时，
//     AS 的粒度也是「一个线程组一个 meshlet」。这个映射让 M18 -> AS 的扩展
//     不需要改动 MS 本身。
//
//   * 线程组 ID（SV_GroupID）直接就是 meshlet 索引 —— 省掉一次索引映射，
//     也让 dispatch 规模（meshlet 数）与数据规模天然对应。
//
// 代价：如果某个 meshlet 顶点数远少于 128（本实现实测最小 18），
// 该线程组就有大量线程闲置。m=13 个 meshlet 时最后一个只有 18 顶点 = 14% 利用率。
// 工业做法是用 Work Graphs / Persistent Threads 做动态负载均衡，
// 但那需要比 M18 更复杂的调度机制。
//
// =============================================================================
// SetMeshOutputCounts：Mesh Shader 存在的根本理由
// =============================================================================
// 传统管线里，一个 draw 要输出多少顶点/三角形是**固化在命令里**的：
//   IndexCountPerInstance = N  ->  顶点着色器固定跑 N 次
//
// Mesh Shader 允许在**运行时**决定：
//
//       SetMeshOutputCounts(vertCount, primCount);
//
// 于是线程组可以先判断「这个 meshlet 到底需不需要画」，
// 需要就设成 128，不需要就设成 0 —— 后者等价于整簇跳过，
// **而且那些顶点的顶点着色器根本不会被执行**。
//
// 这就是「先剔除再组装」相对「先组装再剔除」的本质区别：
// 传统路径下被剔除的三角形仍然要付出 IA 组装 + VS 调用的代价，
// Mesh Shader 下它们**从未存在过**。
//
// 注意：SetMeshOutputCounts 必须被线程组内**所有**线程以相同值调用
// （它是一个组级广播操作），所以本 shader 在分支之外统一调用一次。
//
// =============================================================================
// Meshlet 数据访问（M17 产出的四个缓冲）
// =============================================================================
//   gMeshlets[gid]           该 meshlet 的 4 个偏移/计数
//   gVertIndices[vertOffset + i]   第 i 个**局部槽位** -> 原始顶点缓冲索引
//   gPrimIndices[primOffset + k]   第 k 个三角形，10+10+10 位打包的**局部**索引
//   gBounds[gid]             包围球 + 法线锥（本阶段未用于剔除，留给 Amplification）
//
// 两层索引是 meshlet 压缩的核心：三角形里存的是局部索引（省 2/3 带宽），
// 必须经 gVertIndices 还原成真正的顶点缓冲索引。
//
// =============================================================================
// 与 M17 的 GPU 布局逐字段对应（见 MeshletResources.h）
// =============================================================================

// M17 用的实际上限（MeshletBuilder::kDefaultMaxVerts / kDefaultMaxPrims）。
// 与 DispatchMesh 的线程组大小共同决定输出数组的尺寸。
#define MESHLET_MAX_VERTS 128
#define MESHLET_MAX_PRIMS 128

struct Meshlet
{
    uint vertCount;   // offset  0
    uint vertOffset;  // offset  4
    uint primCount;   // offset  8
    uint primOffset;  // offset 12
};

// 与 MeshletBoundsGPU 对应（stride 32）。M18 只读包围球做可视化/调试，
// 真正的剔除留给 Amplification Shader 阶段。
struct MeshletBounds
{
    float4 boundingSphere; // xyz = 球心, w = 半径
    uint   normalCone;     // 打包 XMUBYTEN4
    float  apexOffset;
    uint   pad0;
    uint   pad1;
};

// 与 Asset/MeshData.h 的 MeshVertex 对应（stride 32）。
// 顶点数据通过 SRV 读取，而不是通过 Input Assembler ——
// Mesh Shader 根本不存在 IA 阶段。
struct MeshVertex
{
    float3 position; // offset  0
    float3 normal;   // offset 12
    float2 uv;       // offset 24
};

StructuredBuffer<Meshlet>       gMeshlets    : register(t0); // 每 meshlet 一条（stride 16）
StructuredBuffer<uint>          gVertIndices : register(t1); // 局部槽位 -> 原始顶点索引（stride 4）
StructuredBuffer<uint>          gPrimIndices : register(t2); // 打包三角形（stride 4）
StructuredBuffer<MeshletBounds> gBounds      : register(t3); // 剔除数据（stride 32）
StructuredBuffer<MeshVertex>    gVertices    : register(t4); // 真正的顶点数据（stride 32）

cbuffer MeshShaderConstants : register(b0)
{
    float4x4 gWorldViewProj;      // 行主序上传 + cbuffer 默认 column-major -> 用 mul(M, v)
    float4   gColor;
    float4   gFrustumPlanes[6];   // (nx, ny, nz, d) —— AS 用；MS 不读
    float4   gEyePosition;        // AS 用；MS 不读
    uint     gMeshletCount;
    uint     gConeCullingEnabled;
    uint     gFrustumCullingEnabled;
    uint     gPad0;
};

// Amplification -> Mesh 的 payload
//
// **必须与 MeshletAS.hlsl 的同名结构逐字段一致** —— HLSL 没有共享头，
// 两边只能手工保持一致（与本项目其它着色器结构的做法相同）。
//
// 这是 M19 相对 M18 的关键变化：
//   M18：MS 组直接用 SV_GroupID 当 meshlet 索引（假设「组号 == meshlet 索引」）
//   M19：MS 组先查 payload 表 —— 因为 AS 会剔除掉一部分 meshlet，
//        组号与 meshlet 索引的对应关系被打破了
#define MAX_MESHLETS_PER_GROUP 32

struct Payload
{
    uint meshletIndices[MAX_MESHLETS_PER_GROUP];
};

struct VertexOut
{
    float4 position : SV_Position; // 必须是 SV_Position，Mesh Shader 直接输出裁剪空间坐标
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
};

// outputtopology("triangle")：
//   声明本线程组输出的图元拓扑。Mesh Shader 必须显式声明，
//   因为不存在 IA 阶段来解释索引缓冲的拓扑语义。
[outputtopology("triangle")]
[numthreads(MESHLET_MAX_VERTS, 1, 1)]
void main(uint  gtid : SV_GroupThreadID, // 组内线程号 0..127
          uint  gid  : SV_GroupID,       // **payload 下标**（M19 起不再是 meshlet 索引）
          in payload Payload payload,    // ← Amplification Shader 传进来的可见 meshlet 表
          out vertices VertexOut verts[MESHLET_MAX_VERTS],
          out indices uint3      tris[MESHLET_MAX_PRIMS])
{
    // -------------------------------------------------------------------------
    // M19：组号 -> meshlet 索引要走一次 payload 查表
    //
    //   M18：dispatch 覆盖全部 meshlet，所以「组号 == meshlet 索引」成立。
    //   M19：AS 已经把可见的 meshlet 稠密压缩进 payload，
    //        所以组号是**可见集合里的序号**，必须查表才能拿到真正的 meshlet 索引。
    //
    //   这个间接层正是「只为可见 meshlet 启动线程组」的代价：
    //   MS 多一次 groupshared 读（极廉价），换来的是被剔除的 meshlet
    //   **完全不启动线程组**。
    // -------------------------------------------------------------------------
    const uint meshletIndex = payload.meshletIndices[gid];

    // 越界保护。
    //
    // AS 保证 payload[0 .. visibleCount-1] 都是有效索引，而 dispatch 规模就等于
    // visibleCount，所以正常情况下这里永远成立。保留它是因为：
    //   * 一旦 AS 的压缩逻辑出错（比如计数器没清零），这里是唯一能挡住越界读的地方；
    //   * 它让 MS 在「被单独调用」（不带 AS）时也有确定行为。
    Meshlet m;
    m.vertCount  = 0u;
    m.vertOffset = 0u;
    m.primCount  = 0u;
    m.primOffset = 0u;

    if (meshletIndex < gMeshletCount)
    {
        m = gMeshlets[meshletIndex];
    }

    // -------------------------------------------------------------------------
    // SetMeshOutputCounts：**运行时**决定输出规模
    //
    // 必须在任何 verts[]/tris[] 写入之前、且由组内所有线程统一调用。
    // 这里声明「本组要输出 m.vertCount 个顶点、m.primCount 个三角形」——
    // 也就是说只有 < m.vertCount 的线程才有权写 verts[]。
    //
    // **注意剔除发生的位置**：整簇级的剔除已经在 AS 里做完了，
    // 所以能走到这里的 meshlet 一定是「通过视锥 + 法线锥测试」的。
    // 这里不再做任何簇级剔除 —— 那属于 AS 的职责。
    // -------------------------------------------------------------------------
    SetMeshOutputCounts(m.vertCount, m.primCount);

    // -------------------------------------------------------------------------
    // ① 输出顶点
    //
    // 第 i 个线程负责第 i 个**局部槽位**。两步索引：
    //   localSlot(i) --gVertIndices--> 原始顶点索引 --gVertices--> 顶点数据
    // -------------------------------------------------------------------------
    if (gtid < m.vertCount)
    {
        const uint globalIndex = gVertIndices[m.vertOffset + gtid];
        const MeshVertex v = gVertices[globalIndex];

        VertexOut o;
        // 行主序矩阵上传 + column-major cbuffer -> mul(M, v) 才是对的（见文件顶部约定）
        o.position = mul(gWorldViewProj, float4(v.position, 1.0));
        o.normal   = v.normal;
        o.uv       = v.uv;

        verts[gtid] = o;
    }

    // -------------------------------------------------------------------------
    // ② 输出三角形索引
    //
    // 第 k 个线程负责第 k 个三角形。gPrimIndices 里每个元素是
    // **三个 10 位局部索引**打包成的 uint32（M17 的 MeshletTriGPU）。
    // 这里手工拆位 —— 位布局必须与 C++ 侧的 static_assert 严格一致。
    // -------------------------------------------------------------------------
    if (gtid < m.primCount)
    {
        const uint packed = gPrimIndices[m.primOffset + gtid];

        const uint i0 = (packed      ) & 0x3FFu; // 低 10 位
        const uint i1 = (packed >> 10) & 0x3FFu; // 中 10 位
        const uint i2 = (packed >> 20) & 0x3FFu; // 高 10 位

        // tris[] 里的索引是**相对于本组输出顶点数组 verts[] 的下标**，
        // 而不是全局顶点索引 —— Mesh Shader 的输出是一个自包含的小网格。
        tris[gtid] = uint3(i0, i1, i2);
    }
}
