// HZBOcclusionCS.hlsl - 用 HZB 做 GPU 遮挡剔除（M15）
//
// 一个线程处理一个**视锥内候选实例**：把它的世界空间包围球投影到屏幕空间，
// 估计屏幕包围矩形，据此选一个 HZB mip 级，采样该区域内的**最远深度**，
// 与物体自身最近深度比较，判定 Visible / Occluded。
//
// =============================================================================
// 保守性（Conservative Behavior）是这整个文件的第一原则
// =============================================================================
//   遮挡剔除**绝不能漏画**。判定「被遮挡」必须是**充分条件**：
//   只有能证明物体完全在已有几何之后时才允许剔除。
//   因此下面每一处不确定的情况都**偏向 Visible**：
//
//     * 球心在相机后方（clip.w <= radius）        -> Visible
//     * 球穿过近平面                              -> Visible
//     * 屏幕矩形完全落在画面外                    -> Visible
//     * mip 级选得比"刚好"更粗（用 ceil）         -> 更保守
//     * 采样矩形向外扩一圈 + 取区域内 max          -> 更保守
//     * 深度比较加 bias                            -> 更保守
//
//   代价是"少剔除一些本该剔除的物体"——这正是我们愿意付的代价。
//
// =============================================================================
// Reduction Convention（与 M14 保持一致，这里再确认一次）
// =============================================================================
//   投影约定：传统 DirectX 深度，Near -> 0、Far -> 1，**深度值越大越远**
//   HZB 归约：**Max**（每个像素 = 它覆盖区域内深度的最大值）
//
//   于是「物体最近深度 > HZB 值」等价于
//        「物体比该区域内**所有**像素都远」
//   即物体被完全遮挡 —— 判定是充分的 ✓
//
//   > 若改成 Reverse-Z，HZB 必须用 Min，这里的比较方向**不变**
//   >（仍然是 objectNear 与 occluder 比较），但语义上 occluder 变成了"最近"。
//
// =============================================================================
// 屏幕空间矩形估计：为什么用 sqrt(w² - r²) 而不是直接用 w
// =============================================================================
//   包围球在屏幕上的投影**不是**一个"中心投影 + 半径/w"的圆 ——
//   球的前表面比球心更靠近相机，投影出来更大。
//
//   精确的投影半径（NDC）：
//       r_ndc = r * P11 / sqrt(d² - r²)
//   其中 d = 球心视空间深度（= clip.w），P11 是投影矩阵的 y 缩放。
//
//   分母里那个 sqrt(d² - r²) 是球与相机的切平面距离 ——
//   用它才能得到**包含**整个球的投影半径（保守的做法）。
//   如果偷懒只除以 d，得到的圆会偏小，边缘像素的判断就会不准。
//
//   当 d <= r 时球包含相机（或相机在球内），此时无法定义屏幕矩形，
//   必须保守地判为 Visible。

struct InstanceData
{
    row_major float4x4 world; // offset  0 (64B)
    float4   boundingSphere;  // offset 64 (16B) xyz = 世界空间球心, w = 半径
    uint     meshIndex;       // offset 80 (4B)
    uint     materialIndex;   // offset 84 (4B)
    uint2    padding;         // offset 88 (8B) -> 结构体补齐到 96
};

StructuredBuffer<InstanceData> gInstances : register(t0);

// 视锥剔除的候选列表（M11 的压缩输出）
StructuredBuffer<uint> gCandidates : register(t1);

// HZB 深度金字塔（M14 生成，Max 归约）
Texture2D<float> gHZBMip0 : register(t2);

// 候选计数器（4 字节）。
//
// 为什么需要它：Dispatch 的规模只能按**容量上限**取整 —— CPU 根本不知道
// 本帧实际有多少候选（去读它就要同步等待 GPU，那正是我们要避免的）。
// 所以每个线程自己读一次真实数量，多出来的线程直接退出。
// 这与 M10/M11 的「按容量 dispatch」是同一个模式。
ByteAddressBuffer gCandidateCount : register(t3);

RWStructuredBuffer<uint> gVisibleIndices : register(u0);
RWByteAddressBuffer gVisibleCount : register(u1);

// 每帧统计（uint 数组，字节偏移）：
//   [ 0] 候选数（= 视锥内实例数）
//   [ 4] 因遮挡被剔除数
//   [ 8] 因"无法安全判定"而保守放行数（用于衡量保守性的代价）
//   [12] 采样的 HZB 像素总数（用于观察 mip 选择是否合理）
RWByteAddressBuffer gStats : register(u2);

cbuffer OcclusionConstants : register(b0)
{
    // 注意：cbuffer 默认 column-major，HLSL 读到的 gViewProj 是 CPU 上传矩阵的转置，
    // 所以下面必须用 mul(matrix, vector) —— 与 MeshGPUDrivenVS.hlsl 的约定一致。
    float4x4 gViewProj;

    float2 gResolution;    // 全分辨率（深度缓冲）尺寸
    float2 gHZBInvSize;    // HZB mip 0 的 1/尺寸

    float gProj00;         // 投影矩阵 [0][0]（x 缩放）
    float gProj11;         // 投影矩阵 [1][1]（y 缩放）
    float gNearPlane;
    float gFarPlane;

    float gDepthBias;      // 深度比较的保守偏置（NDC 单位）
    uint  gHZBMipCount;
    uint  gCandidateCapacity; // 仅用于校验线程范围，真实数量从 gCandidateCount 读
    uint  gPad0;
};

// 统计缓冲里的 DWORD 偏移（与 HZBOcclusionCuller::StatsOffset 对应）
static const uint kStatCandidateCountOffset = 0u;
static const uint kStatOccludedCountOffset = 4u;
static const uint kStatConservativePassOffset = 8u;
static const uint kStatSampledTexelsOffset = 12u;

// 一个 HZB 像素覆盖多少全分辨率像素：mip k 覆盖 2^(k+1) × 2^(k+1)
float HZBTexelScale(uint mip)
{
    return exp2((float)mip + 1.0);
}

void AppendVisible(uint instanceIndex)
{
    uint outputIndex = 0u;
    gVisibleCount.InterlockedAdd(0u, 1u, outputIndex);
    gVisibleIndices[outputIndex] = instanceIndex;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint slot = dispatchThreadId.x;

    // 真实候选数由 GPU 侧的压缩计数器给出（见 gCandidateCount 的说明）
    const uint candidateCount = gCandidateCount.Load(0u);
    if (slot >= candidateCount)
    {
        return;
    }

    // 只有 0 号线程负责把候选数记进统计，避免 N 次原子加
    if (slot == 0u)
    {
        gStats.Store(kStatCandidateCountOffset, candidateCount);
    }

    // 候选列表是压缩过的，必须经过一次间接寻址拿到真正的实例下标
    const uint instanceIndex = gCandidates[slot];
    const InstanceData instance = gInstances[instanceIndex];
    const float3 center = instance.boundingSphere.xyz;
    const float radius = instance.boundingSphere.w;

    // ---- 1. 投影到裁剪空间 ----
    const float4 clip = mul(gViewProj, float4(center, 1.0));

    // 球心在相机后方 / 球包含相机：屏幕矩形无定义 -> 保守放行
    if (clip.w <= radius)
    {
        gStats.InterlockedAdd(kStatConservativePassOffset, 1u); // conservativePass++
        AppendVisible(instanceIndex);
        return;
    }

    // ---- 2. 估计屏幕空间包围矩形 ----
    //
    // NDC 半径：分母用 sqrt(d² - r²)，见文件顶部的推导。
    const float depthSq = clip.w * clip.w - radius * radius;
    const float denom = sqrt(max(depthSq, 1e-6));

    const float2 ndcCenter = clip.xy / clip.w;
    const float2 ndcRadius = float2(radius * gProj00 / denom,
                                    radius * gProj11 / denom);

    // NDC -> 屏幕像素。注意 y 翻转：NDC 的 +y 向上，屏幕的 +y 向下。
    const float2 halfRes = gResolution * 0.5;
    const float2 ndcMin = ndcCenter - ndcRadius;
    const float2 ndcMax = ndcCenter + ndcRadius;

    const float2 screenMin = float2((ndcMin.x * 0.5 + 0.5) * gResolution.x,
                                    (0.5 - ndcMax.y * 0.5) * gResolution.y);
    const float2 screenMax = float2((ndcMax.x * 0.5 + 0.5) * gResolution.x,
                                    (0.5 - ndcMin.y * 0.5) * gResolution.y);

    // 矩形完全在画面外：无法用 HZB 判定 -> 保守放行。
    // 这一条同时保证了「物体在屏幕边缘不会被随机剔除」：
    // 只要矩形与屏幕有任何重叠，我们就继续走完整的遮挡测试。
    if (screenMax.x <= 0.0f || screenMin.x >= gResolution.x ||
        screenMax.y <= 0.0f || screenMin.y >= gResolution.y)
    {
        gStats.InterlockedAdd(kStatConservativePassOffset, 1u);
        AppendVisible(instanceIndex);
        return;
    }

    // ---- 3. 按投影尺寸选择 mip 级 ----
    //
    // 目标：让矩形在选定 mip 上大约覆盖 1~2 个 HZB 像素。
    // HZB mip k 的一个像素覆盖 2^(k+1) 个全分辨率像素，所以
    //     k ≈ ceil(log2(矩形较大边)) - 1
    //
    // **用 ceil 而不是 floor**：宁可取更粗的一级（一个像素覆盖更大区域），
    // 这样采到的 max 只会更大 -> 更难判为遮挡 -> 保守 ✓
    const float2 rectSize = max(screenMax - screenMin, float2(1.0f, 1.0f));
    const float largerEdge = max(rectSize.x, rectSize.y);
    float mipF = ceil(log2(largerEdge)) - 1.0f;
    mipF = clamp(mipF, 0.0f, (float)(gHZBMipCount - 1u));
    const uint mip = (uint)mipF;

    // ---- 4. 保守采样：区域内所有 HZB 像素取 max ----
    const float scale = HZBTexelScale(mip);
    const float2 hzbSize = 1.0f / gHZBInvSize; // mip 0 的像素尺寸

    // 矩形在选定 mip 上的范围，向外扩 1 个像素以确保完全覆盖
    float2 mipMin = screenMin / scale;
    float2 mipMax = screenMax / scale;

    int2 p0 = int2(floor(mipMin)) - 1;
    int2 p1 = int2(ceil(mipMax)) + 1;

    // clamp 到 HZB 边界。
    // 注意：屏幕外的部分被裁掉了 —— 那部分深度是无效的（背景），
    // 把它算进来会让 occluder 偏大、误判为"更远"，反而不保守。
    p0 = clamp(p0, int2(0, 0), int2(hzbSize) - 1);
    p1 = clamp(p1, int2(0, 0), int2(hzbSize) - 1);

    // 双保险：限制采样上限，避免屏幕外大矩形被 clamp 后导致循环过长
    if (p1.x - p0.x > 3) { p1.x = p0.x + 3; }
    if (p1.y - p0.y > 3) { p1.y = p0.y + 3; }

    float occluderDepth = 0.0f;
    uint sampleCount = 0u;
    for (int y = p0.y; y <= p1.y; ++y)
    {
        for (int x = p0.x; x <= p1.x; ++x)
        {
            occluderDepth = max(occluderDepth, gHZBMip0.Load(int3(x, y, mip)));
            ++sampleCount;
        }
    }
    gStats.InterlockedAdd(kStatSampledTexelsOffset, sampleCount);

    // ---- 5. 物体最近深度 ----
    //
    // 球的最近点 = 球心视空间深度 - 半径（clip.w 对标准投影就是视空间深度）。
    const float objectNearView = clip.w - radius;
    if (objectNearView <= gNearPlane)
    {
        // 球穿过近平面：它的最近点比近平面还近，投影深度会退化 -> 保守放行
        gStats.InterlockedAdd(kStatConservativePassOffset, 1u);
        AppendVisible(instanceIndex);
        return;
    }

    // 视空间深度 -> NDC 深度（传统 D3D 投影的解析式）
    //     z_ndc = far * (z_view - near) / (z_view * (far - near))
    const float objectNearNdc = gFarPlane * (objectNearView - gNearPlane) /
                                (objectNearView * (gFarPlane - gNearPlane));

    // ---- 6. 判定 ----
    //
    // 保守：只有**严格**更远（且超过 bias）才剔除。
    // bias 用来吸收浮点误差 —— 没有它，深度恰好相等的表面可能被误剔除。
    if (objectNearNdc > occluderDepth + gDepthBias)
    {
        gStats.InterlockedAdd(kStatOccludedCountOffset, 1u); // occlusionCulled++
        return;
    }

    AppendVisible(instanceIndex);
}
