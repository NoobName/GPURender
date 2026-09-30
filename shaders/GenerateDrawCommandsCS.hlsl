// GenerateDrawCommandsCS.hlsl - 生成**一条** DrawIndexed 间接命令（M12）
//
// 这是「GPU-Driven Rendering」里把**可见性结果**翻译成**绘制命令**的一步：
// 它只写一条命令，但把 InstanceCount 设成 GPU 侧算出来的可见数 ——
// 于是「画多少个实例」这个决定完全留在 GPU 上。
//
// -----------------------------------------------------------------------------
// 关于 StartInstanceLocation：一个被证伪的假设（务必先读）
// -----------------------------------------------------------------------------
//   本版最初的想法是「每个可见实例生成一条命令，用 StartInstanceLocation
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
//   结果全部画成了实例 0（1570 个重叠在一起，看起来只有一个方块），
//   且不报任何错。
//
// -----------------------------------------------------------------------------
// 改用「一条命令 + 顶点着色器查表」
// -----------------------------------------------------------------------------
//   * 命令只有一条：IndexCountPerInstance = 几何索引数，
//     **InstanceCount = GPU 算出的可见数**，StartInstanceLocation = 0；
//   * 顶点着色器拿到的 SV_InstanceID 是 0..visibleCount-1，
//     用它去索引 M11 产出的压缩列表，得到真正的实例 ID。
//
//   好处：ExecuteIndirect 的开销与可见数无关（永远只有一条命令），
//         而且 MaxCommandCount 是编译期已知的常数 1。
//   代价：顶点着色器多一次间接寻址（几百纳秒级，可忽略）。

static const uint kDrawIndexedArgumentSize = 20u;

ByteAddressBuffer   gVisibleCount      : register(t1);
RWByteAddressBuffer gIndirectArguments : register(u0);

cbuffer CommandConstants : register(b0)
{
    uint gMaxCommands;   // 不再使用（只有一条命令），保留以便将来扩展
    uint gIndexCount;    // 每个实例要画的索引数
    uint gPad0;
    uint gPad1;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // 只需要一个线程写这一条命令。
    if (dispatchThreadId.x != 0u)
    {
        return;
    }

    // 可见数来自 GPU 的计数器。CPU 从不读它 —— 它只是 GPU 内部的一份数据。
    // 这里就是「命令条数由 GPU 决定」的落点。
    const uint visibleCount = gVisibleCount.Load(0u);

    gIndirectArguments.Store(0u,  gIndexCount);  // IndexCountPerInstance
    gIndirectArguments.Store(4u,  visibleCount); // InstanceCount  <- GPU 决定
    gIndirectArguments.Store(8u,  0u);           // StartIndexLocation
    gIndirectArguments.Store(12u, 0u);           // BaseVertexLocation
    gIndirectArguments.Store(16u, 0u);           // StartInstanceLocation

    (void)gMaxCommands;
}
