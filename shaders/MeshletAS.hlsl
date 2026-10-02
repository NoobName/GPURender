// MeshletAS.hlsl - Amplification Shader：Meshlet 级剔除 + 压缩 + 按需发射（M19）
//
// =============================================================================
// 它解决什么问题
// =============================================================================
//   M18 的 Mesh Shader 路径虽然把几何组装交给了 GPU，但**每个 meshlet 都会启动
//   一个 Mesh Shader 线程组** —— 包括那些完全看不见的。
//
//   Amplification Shader 插在它们前面：先把 meshlet 剔除一遍，
//   只把**通过剔除的**写进 payload，然后用 payload 的规模去 DispatchMesh。
//
//   于是被剔除的 meshlet **根本不会启动 Mesh Shader 线程组** ——
//   不是「启动了但提前返回」，而是**从未存在过**。
//   这是 M19 相对 M18 的全部意义。
//
// =============================================================================
// 执行流程
// =============================================================================
//   CPU: DispatchMesh(ceil(meshletCount / AS_GROUP_SIZE), 1, 1)
//          │
//          ▼
//   ┌─────────────────────────────────────────────────────────────┐
//   │ AS 线程组（AS_GROUP_SIZE = 32 线程）                          │
//   │   一个线程负责一个 meshlet                                    │
//   │                                                              │
//   │   ① 视锥剔除      包围球 vs 6 个平面                          │
//   │   ② 法线锥剔除    整簇朝向是否背对相机                         │
//   │   ③ 压缩          通过的 meshlet 用原子加拿到 payload 槽位      │
//   │   ④ DispatchMesh(visibleCount, 1, 1, payload)                │
//   └─────────────────────────────────────────────────────────────┘
//          │
//          ▼
//   MS 线程组 × visibleCount（只为可见的 meshlet 启动）
//
// =============================================================================
// Mesh Shader Payload 是什么
// =============================================================================
//   payload 是 Amplification Shader **传给**它所发射的那些 Mesh Shader 线程组的
//   一块**组共享**数据。它有三个关键性质：
//
//   1. **存在 groupshared 内存里** —— 不是显存，不是寄存器，访问延迟极低；
//   2. **被所有由本次 DispatchMesh 发射的 MS 线程组共享** —— 所以里面存的是
//      「这一批可见 meshlet 的索引表」，每个 MS 组按下标取自己那一个；
//   3. **有硬性大小上限（16 KB）** —— 所以 payload 必须**紧凑**。
//
//   本实现里 payload 就是一个 uint 数组（可见 meshlet 索引表）：
//       payload.meshletIndices[i] = 第 i 个可见 meshlet 的全局索引
//   MS 组用 SV_GroupID 作为 i 去查表，于是 **MS 不再假设「组号 == meshlet 索引」**。
//
//   这正是「Compact Payload」的含义：把稀疏的可见集合压成一个稠密数组，
//   让 MS 的 dispatch 规模恰好等于可见数。
//
// =============================================================================
// 法线锥剔除（Normal Cone / Backface Culling）
// =============================================================================
//   Meshlet 的剔除数据里有一个**法线锥**：锥轴 A + 半角 α，
//   保证该 meshlet 内**所有**三角形法线 n 都满足 dot(n, A) >= cos(α)。
//
//   一次测试就能判断「整簇是否背对相机」：
//
//       apex = center - A * apexOffset       // DirectXMesh 的保守锥顶点
//       V = normalize(apex - eye)           // 从眼睛指向锥顶点的方向
//       剔除  <=>  dot(V, A) > sin(α)
//
//   直觉：V 与 A 越同向，说明锥轴顺着视线指出去 —— 也就是这簇面朝外。
//   当这个点积超过 sin(α) 时，可以证明锥内**最不利的那个法线**也仍然背离视线，
//   所以整簇一定背面朝外，可以整块丢掉。
//
//   apexOffset 保证锥顶点位于该簇所有三角形平面的内侧；透视视线必须从
//   该顶点计算，仅用包围球中心不具备这一保证。所有量统一在模型空间。
//   为什么这是**保守**的（不会误剔）：
//     设 c = dot(V,A)，则锥内 dot(n,V) 的最小值是
//         c·cos(α) - sin(α)·sqrt(1 - c²)
//     在 c > sin(α) 的条件下可以推出该最小值 > 0，
//     即「所有法线都背离视线」成立 —— 所以剔除是充分的。
//
//   极端情形自检：
//     α = 0（所有法线相同）-> sin(α) = 0 -> 剔除条件退化成 dot(V,A) > 0，正确
//     α = 90°（锥覆盖整个半球）-> sin(α) = 1 -> 永远不剔除，正确
//
//   DirectXMesh 把锥轴编码成 signed byte、把 sin(α) 编码成 unsigned byte，
//   一起打包进一个 uint32（见下方解码）。
// =============================================================================

// 一个 AS 线程组处理 32 个 meshlet —— 与 numthreads 一致（一线程一 meshlet）。
// 必须是 wave 的倍数（32），这样组内正好一个 wave，压缩时不需要多余的同步。
#define AS_GROUP_SIZE 32

// payload 容量 == 组大小（最坏情况本组 32 个 meshlet 全部可见）。
// 32 个 uint = 128 字节，远低于 16 KB 上限。
#define MAX_MESHLETS_PER_GROUP AS_GROUP_SIZE

// -----------------------------------------------------------------------------
// 与 MeshletMS.hlsl 逐字段一致（HLSL 没有共享头，两边必须手工保持一致）
// -----------------------------------------------------------------------------
struct Meshlet
{
    uint vertCount;
    uint vertOffset;
    uint primCount;
    uint primOffset;
};

struct MeshletBounds
{
    float4 boundingSphere; // xyz = 球心, w = 半径
    uint   normalCone;     // 打包 XMUBYTEN4
    float  apexOffset;
    uint   pad0;
    uint   pad1;
};

// Amplification -> Mesh 的 payload
struct Payload
{
    uint meshletIndices[MAX_MESHLETS_PER_GROUP];
};

StructuredBuffer<Meshlet>       gMeshlets : register(t0);
StructuredBuffer<MeshletBounds> gBounds   : register(t3);
// 注意：AS 只需要 gMeshlets 与 gBounds —— t1/t2/t4 是给 MS 用的，
// 但两者共享同一个根签名，所以 AS 也「看得到」它们，只是不读。
// 这不会带来任何开销：根签名描述的是**可访问范围**，不是实际访问。

// 剔除统计 + 每 meshlet 状态：
//   [ 0.. 3]  视锥剔除数 / 法线锥剔除数 / 可见 meshlet 数 / 可见三角形数（原子累加）
//   [16..  ]  每 meshlet 的剔除状态（0=可见 1=视锥剔除 2=法线锥剔除），供调试可视化
RWByteAddressBuffer gCullStats : register(u0);

cbuffer MeshShaderConstants : register(b0)
{
    float4x4 gWorldViewProj;      // 行主序上传 + cbuffer 默认 column-major -> 用 mul(M, v)
    float4   gColor;
    float4   gFrustumPlanes[6];   // 模型空间 (nx, ny, nz, d)，法线已归一化
    float4   gEyePosition;        // 模型空间相机位置（与 gBounds 一致）
    uint     gMeshletCount;
    uint     gConeCullingEnabled; // 便于 A/B 对比「开/关法线锥剔除」
    uint     gFrustumCullingEnabled;
    uint     gPad0;
};

// -----------------------------------------------------------------------------
// 组共享存储
// -----------------------------------------------------------------------------
// 压缩用的计数器
groupshared uint gsVisibleCount;

// **payload 本体**（见下方长注释：为什么不用 `in payload` 参数形式）
groupshared Payload gPayload;

// -----------------------------------------------------------------------------
// 保守视锥测试：只有包围球**完全**落在某个平面外侧才判为剔除
// （与 M11 的 FrustumCullingCS 同一套判据，保证两条路径结果可比）
// -----------------------------------------------------------------------------
bool IsFrustumCulled(float3 center, float radius)
{
    if (gFrustumCullingEnabled == 0u)
    {
        return false;
    }

    [unroll]
    for (uint i = 0u; i < 6u; ++i)
    {
        const float4 plane = gFrustumPlanes[i];
        const float d = plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w;
        if (d < -radius)
        {
            return true;
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// 法线锥剔除：整簇是否背对相机（推导见文件顶部）
// -----------------------------------------------------------------------------
bool IsConeCulled(float3 center, uint packedCone, float apexOffset)
{
    if (gConeCullingEnabled == 0u)
    {
        return false;
    }

    // 解码打包的 XMUBYTEN4：
    //   xyz 是 signed normalized byte（-1..1），DirectXMesh 写入时偏置了 +128
    //   w   是 unsigned normalized byte（0..1），即 sin(锥半角)
    const int bx = int((packedCone      ) & 0xFFu) - 128;
    const int by = int((packedCone >>  8) & 0xFFu) - 128;
    const int bz = int((packedCone >> 16) & 0xFFu) - 128;
    const uint bw =      (packedCone >> 24) & 0xFFu;

    // w=255 是 DirectXMesh 的退化锥标记；不能依赖解码后的轴长度识别它。
    if (bw == 255u)
    {
        return false;
    }

    const float3 axis = max(float3(float(bx), float(by), float(bz)) / 127.0, -1.0);
    const float  coneCutoff = float(bw) / 255.0;

    // 无效轴或相机位于锥顶点时保守放行，避免归一化产生 NaN。
    const float axisLenSq = dot(axis, axis);
    if (axisLenSq < 1e-6)
    {
        return false;
    }

    const float3 unitAxis = axis / sqrt(axisLenSq);
    const float3 apex = center - unitAxis * apexOffset;
    const float3 eyeToApex = apex - gEyePosition.xyz;
    const float viewLenSq = dot(eyeToApex, eyeToApex);
    if (viewLenSq < 1e-12)
    {
        return false;
    }
    const float3 viewDir = eyeToApex / sqrt(viewLenSq);

    return dot(viewDir, unitAxis) > coneCutoff + 1e-5;
}

[numthreads(AS_GROUP_SIZE, 1, 1)]
void main(uint  gtid : SV_GroupThreadID,
          uint  gid  : SV_GroupID)
{
    // =========================================================================
    // 为什么这里没有 `in payload Payload payload` 参数 —— 一个真实的工具链坑
    // =========================================================================
    // 规范写法是让 AS 在入口接收 payload 对象：
    //
    //     void main(uint gtid : SV_GroupThreadID, uint gid : SV_GroupID,
    //               in payload Payload payload)
    //
    // **但本机的 DXC 1.8.2502.11（Windows SDK 10.0.26100.0）生成的 DXIL 过不了
    // DXIL 验证器**，报：
    //
    //     error: Function main with parameter is not permitted, it should be inlined.
    //     error: Type 'Payload' is a struct type but is used as a parameter in
    //            function 'main'.
    //
    // 用 `-Vd`（关闭验证）dump 出 DXIL 就能看到根因 —— payload 被留成了**普通函数参数**：
    //
    //     define void @main(i32 %gtid, i32 %gid, %struct.P* %payload)
    //     !4 = !{void (i32, i32, %struct.P*)* @main, ...}
    //
    // 而规范要求 payload 位于 **TGSM（groupshared）**，入口签名里不应出现它。
    // 也就是说：**DXC 没有正确地把 payload 降级成 TGSM 全局变量**。
    // 这是 DXC 里一类已知的 AS payload 降级问题，与 as_6_5/6_6/6_8 无关。
    //
    // **绕过办法**：自己用 `groupshared` 声明 payload 变量，
    // 入口签名保持干净，把它直接传给 DispatchMesh：
    //
    //     define void @main()                                    <- 干净
    //     call void @dx.op.dispatchMesh....(%struct.P addrspace(3)* @"?gPayload@@...")
    //                                                            ^^^^ TGSM 指针，正确
    //
    // 生成的形态与规范一致，验证器通过。
    // 代价：payload 变量要自己声明，而且**必须与 MS 侧 `in payload` 的类型逐字段一致**
    // （TGSM 布局是两边唯一的契约）。
    // =========================================================================

    // 本线程负责的 meshlet 全局索引
    const uint meshletIndex = gid * AS_GROUP_SIZE + gtid;

    // 越界线程（最后一组可能有富余）一律视为不可见
    bool inRange = (meshletIndex < gMeshletCount);

    bool frustumCulled = false;
    bool coneCulled    = false;

    if (inRange)
    {
        const MeshletBounds b = gBounds[meshletIndex];
        const float3 center = b.boundingSphere.xyz;
        const float  radius = b.boundingSphere.w;

        frustumCulled = IsFrustumCulled(center, radius);
        if (!frustumCulled)
        {
            coneCulled = IsConeCulled(center, b.normalCone, b.apexOffset);
        }
    }

    const bool visible = inRange && !frustumCulled && !coneCulled;

    // -------------------------------------------------------------------------
    // 统计：三类各记一次
    //
    // 用原子加而不是「每线程写自己的槽位再归约」—— 统计量本身没有顺序要求，
    // 原子加最简单且不需要额外的归约步骤。
    // -------------------------------------------------------------------------
    if (frustumCulled)
    {
        gCullStats.InterlockedAdd(0u, 1u);
    }
    if (coneCulled)
    {
        gCullStats.InterlockedAdd(4u, 1u);
    }
    if (visible)
    {
        gCullStats.InterlockedAdd(8u, 1u);

        // 可见三角形数：累加该 meshlet 的三角形数。
        // 这个数字直接回答「这一帧到底组装了多少几何」——
        // 与「全场三角形数」的比值就是剔除的实际收益。
        const Meshlet m = gMeshlets[meshletIndex];
        gCullStats.InterlockedAdd(12u, m.primCount);
    }

    // -------------------------------------------------------------------------
    // 每 meshlet 的剔除状态 —— 供 CPU 侧调试可视化使用
    //
    //   偏移 16 + i*4 : 第 i 个 meshlet 的状态
    //   0 = 可见   1 = 被视锥剔除   2 = 被法线锥剔除
    //
    // 为什么用 Store 而不是原子加：每个线程写**自己**的槽位，互不冲突。
    // 这份数据让画面上能直接把三类 meshlet 用不同颜色标出来 ——
    // 只看统计数字无法判断「剔除的位置对不对」，颜色可以。
    // -------------------------------------------------------------------------
    if (inRange)
    {
        const uint status = frustumCulled ? 1u : (coneCulled ? 2u : 0u);
        gCullStats.Store(16u + meshletIndex * 4u, status);
    }

    // -------------------------------------------------------------------------
    // 压缩（Stream Compaction）：把可见 meshlet 稠密地写进 payload
    //
    // InterlockedAdd 返回**加之前的旧值**，恰好就是本线程在可见集合里的序号。
    // 一个 wave（32 线程）内的原子加由硬件高效处理，不需要额外的同步。
    // -------------------------------------------------------------------------
    if (gtid == 0u)
    {
        gsVisibleCount = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    if (visible)
    {
        uint slot = 0u;
        InterlockedAdd(gsVisibleCount, 1u, slot);
        gPayload.meshletIndices[slot] = meshletIndex;
    }

    // payload 会被 MS 读取，必须等所有线程写完
    GroupMemoryBarrierWithGroupSync();

    // -------------------------------------------------------------------------
    // 发射 Mesh Shader 线程组 —— **只为可见的 meshlet**
    //
    // 参数 visibleCount 就是门槛：
    //   * 若本组有 5 个 meshlet 通过剔除 -> 只启动 5 个 MS 线程组
    //   * 若一个都没通过 -> visibleCount = 0，**一个 MS 线程组都不会启动**
    //
    // 这就是验收标准「Invisible Meshlet 不会启动不必要的 Mesh Shader Work」的落点。
    // 注意它不是「启动了然后立刻 return」—— 被剔除的 meshlet 的顶点着色**从未被执行过**。
    //
    // **必须由组内所有线程调用，不能包在 `if (gtid == 0)` 里。**
    //   DXC 会直接报错：`Non-Dominating DispatchMesh call`。
    //   原因：DispatchMesh 是**组级广播操作**（与 MS 里的 SetMeshOutputCounts 同类），
    //   shader 编译器要求它处于「支配所有出口路径」的统一控制流中。
    //   放在线程分支里会让控制流不统一 —— 即使运行时只有线程 0 真正生效也不允许。
    //
    //   这是我最初写错、并被编译器纠正的一点：直觉上「一个线程发出就够了」，
    //   但规范要求的是**全组一致地调用**。
    //
    // 此时 gsVisibleCount 已被上面的 GroupMemoryBarrierWithGroupSync 同步到全组可见，
    // 所以所有线程读到的是同一个值。
    // -------------------------------------------------------------------------
    DispatchMesh(gsVisibleCount, 1, 1, gPayload);
}
