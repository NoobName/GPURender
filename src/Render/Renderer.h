#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "Render/Camera.h"
#include "Render/DebugLines.h"
#include "Render/DebugText.h"
#include "Render/GPUBuffer.h"
#include "Render/GPUProfiler.h"
#include "Render/GPUFrustumCuller.h"
#include "Render/MeshLOD.h"
#include "Render/MeshletBuilder.h"
#include "Render/MeshletResources.h"
#include "Render/HierarchicalZBuffer.h"
#include "Render/HZBOcclusionCuller.h"
#include "Render/IndirectDrawCommands.h"
#include "Render/InstanceBuffer.h"
#include "Render/InstanceValidator.h"
#include "Render/VisibleInstanceList.h"
#include "Render/Material.h"
#include "Render/Mesh.h"
#include "Render/Texture.h"
#include "Scene/Scene.h"

using Microsoft::WRL::ComPtr;

// 每个实例每帧的常量数据。
//
// 布局（144 字节，实际按 256 字节占一个槽位）：
//   offset   0 : model          float4x4 -> 模型 -> 世界
//   offset  64 : viewProj       float4x4 -> 世界 -> 裁剪
//   offset 128 : lightDirection float4   -> 世界空间光照方向
//
// 注意这里**没有** boundingSphere / meshIndex 等场景数据 —— 那些留在 CPU 侧的
// InstanceData 里。M7 是 CPU-Driven 渲染，场景数据不需要上传；等到 M8 把实例数据
// 常驻 GPU 时，才会把 InstanceData 原样打包进 StructuredBuffer。
struct InstanceConstants
{
    DirectX::XMFLOAT4X4 model;
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT4 lightDirection;
};

// GPU-Driven 路径的**全局**常量（每帧一份，而不是每实例一份）。
//
// 为什么需要它：GPU-Driven 下实例的 world 矩阵来自常驻显存的 StructuredBuffer，
// 但 viewProj 与光照方向是「全场共用」的 —— 它们不适合放在实例缓冲里（会重复 N 份），
// 也不再有每实例的 ObjectConstants 可以读。所以单独开一份每帧常量。
//
// 内存布局（与 shaders/MeshGPUDrivenVS.hlsl / PS.hlsl 的 GlobalConstants 对应）：
//   offset  0 : viewProj       float4x4 -> 世界 -> 裁剪
//   offset 64 : lightDirection float4   -> 世界空间光照方向
struct GlobalConstants
{
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT4 lightDirection;
};

// 一帧的 CPU 侧统计量。M7 建立了 baseline，M8 把「剔除」单独拆出来计量，
// 以便量化 CPU 剔除本身的开销与它带来的提交量下降。
struct FrameStats
{
    double cpuFrameMs = 0.0;   // 整帧 CPU 时间（帧开始 -> 命令提交完毕）
    double cullMs = 0.0;       // **CPU 视锥测试**（M8 新增，独立计时）
    double updateMs = 0.0;     // 写实例常量 + 生成 UI 文本
    double recordMs = 0.0;     // 记录命令（draw submission）—— baseline 的主角
    double presentMs = 0.0;    // Present 调用耗时（开启 vsync 时包含等待显示刷新）

    std::uint32_t totalInstances = 0;     // 场景实例总数
    std::uint32_t visibleInstances = 0;   // 通过 CPU 视锥测试的实例数
    std::uint32_t culledInstances = 0;    // 被剔除的实例数 = total - visible
    std::uint32_t submittedDrawCalls = 0; // 本帧提交的 draw call 数

    // M12：GPU-Driven 模式下 CPU 提交的是「1 次 ExecuteIndirect」，
    // 真正的命令条数由 GPU 的计数器决定 —— 这两个量刻意分开统计，
    // 因为「CPU 提交了几次」正是本里程碑要压缩的对象。
    std::uint32_t indirectExecuteCount = 0; // CPU 侧 ExecuteIndirect 调用次数（0 或 1）
    bool gpuDriven = false;                 // 本帧用的是哪条渲染路径

    // M18：Mesh Shader 路径的统计。
    // 与 indirectExecuteCount 刻意分开 —— 「DispatchMesh 了几次」和
    // 「ExecuteIndirect 了几次」是两条完全不同的提交机制，混在一起就失去意义。
    bool meshShaderPath = false;             // 本帧走的是 Mesh Shader 路径
    std::uint32_t meshShaderDispatchCount = 0; // CPU 侧 DispatchMesh 调用次数（恒为 0 或 1）
    std::uint32_t meshletsDispatched = 0;      // 本次 DispatchMesh 覆盖的 meshlet 数
    std::uint32_t asGroupCount = 0;            // M19：AS 线程组数 = ceil(meshlet数 / 32)
};

// 每帧独立的资源与同步状态。
//
// 为什么需要这个结构：CommandAllocator 是「记录命令的内存池」，只要 GPU 还在
// 执行它里面记录的命令，就不能 Reset。因此给每个 frame 一份独立的 allocator，
// 并在重用某份之前通过 Fence 确认 GPU 已经完成了它上一轮的工作。
struct FrameContext
{
    ComPtr<ID3D12CommandAllocator> commandAllocator; // 该帧独占的命令分配器
    UINT64 fenceValue = 0;                           // 该帧提交后对应的 Fence 值

    // 实例常量缓冲（Upload Heap），按 256 字节切成 kMaxInstances + 1 个槽位：
    //   槽位 0 .. N-1            : 各实例的 InstanceConstants
    //   槽位 kMaxInstances       : UI 叠加层的正交投影常量
    // 之所以按**最大**实例数预分配，是为了让运行时切换 1k/10k/100k 时不需重建资源。
    GPUBuffer constantBuffer;
};

// CPU-Driven 渲染基线渲染器。
//
// M7 的定位：在 M6 的网格/材质/纹理能力之上，建立一个**大规模实例场景**，
// 并用最朴素的「CPU 循环逐个提交 draw call」方式渲染它 —— 这就是后续所有
// GPU-Driven 优化的对照基准。
//
// 刻意不做的事：GPU 剔除、间接绘制、实例数据常驻 GPU。这些是 M8 之后的内容。
class Renderer
{
public:
    static constexpr UINT kFrameCount = 3;         // 三重缓冲
    static constexpr UINT kMaxTextures = 56;       // CBV/SRV/UAV 描述符堆容量

    // 描述符堆里的槽位分配（CBV_SRV_UAV 是同一种堆类型，SRV 与 UAV 混排）
    //
    // 约束：放进**同一张描述符表**的描述符必须在堆里连续
    // （表只指向第一项，硬件按 Range 数量往后取）。下面用注释标出各张表。
    static constexpr UINT kStressTextureSrvSlot = 0;      // 材质纹理          [CPU-Driven 表 / GPU-Driven 材质表]
    static constexpr UINT kFontAtlasSrvSlot = 1;          // 屏幕文本字体图集  [UI 表]
    static constexpr UINT kInstanceSrvSlot = 2;           // StructuredBuffer<InstanceData>（M9）[GPU-Driven 实例表]
    static constexpr UINT kVisibleIndicesSrvSlot = 3;     // 可见索引列表 SRV（M12）  [命令生成表: 3,4]
    static constexpr UINT kVisibleCountSrvSlot = 4;       // 可见计数器 SRV（M12）    [命令生成表: 3,4]
    static constexpr UINT kValidationResultsUavSlot = 5;  // 验证：每实例错误码        [验证表: 5,6]
    static constexpr UINT kValidationDumpUavSlot = 6;     // 验证：原始字节 dump       [验证表: 5,6]
    static constexpr UINT kVisibilityUavSlot = 7;         // 可见索引 UAV（M11）       [剔除表: 7,8]
    static constexpr UINT kVisibleCountUavSlot = 8;       // 可见计数 UAV（M11）       [剔除表: 7,8]
    static constexpr UINT kIndirectArgsUavSlot = 9;       // 间接命令参数 UAV（M12）
    static constexpr UINT kDepthSrvSlot = 10;             // 深度缓冲 SRV（M13，可视化 + HZB 输入）
    // M14：HZB 每个 mip 一个 UAV 描述符，必须**连续**排列 ——
    // 降采样 CS 用一张覆盖 2 个描述符的表同时绑定「源 mip / 目标 mip」。
    static constexpr UINT kHZBFirstMipUavSlot = 11;
    static constexpr UINT kHZBSrvSlot = kHZBFirstMipUavSlot + HierarchicalZBuffer::kMaxMips; // 27
    // M15：遮挡剔除
    //   28 = 统计 UAV；29..32 = 遮挡剔除的可见列表（UAV/SRV）；33..37 = occlusion CS 的表
    static constexpr UINT kOcclusionStatsUavSlot = 28;
    static constexpr UINT kOcclusionStatsSrvSlot = 33;
    // M16：LOD
    //   38 = LOD 元数据 SRV（MeshLODRange 数组）
    //   39..42 = **每级一个**分段 SRV —— 指向 per-LOD 索引列表里对应的那一段。
    //
    //   为什么按段建 SRV，而不是让顶点着色器用 root constant 算偏移：
    //   ExecuteIndirect 一次可以提交多条命令，中间**没有 CPU 介入的机会**，
    //   所以「当前画的是第几级」这个信息没法在两次 draw 之间用 root constant 传进去。
    //   按段建 SRV 之后，每次 ExecuteIndirect 之前换一次描述符表即可，
    //   **顶点着色器完全不需要知道 LOD 的存在**（SV_InstanceID 仍然从 0 开始索引单段列表）。
    static constexpr UINT kLODMetadataSrvSlot = 38;
    static constexpr UINT kLODSegmentSrvSlot = 39;
    // 每实例 LOD UAV（M16，仅可视化用）。由 GPUFrustumCuller 写入。
    static constexpr UINT kInstanceLodUavSlot = 43;

    // M17：Meshlet 的 4 个 SRV，**必须连续**（MeshletResources 按 firstSrvSlot + offset 计算）。
    //   44 = MeshletGPU 数组
    //   45 = UniqueVertexIndices
    //   46 = PrimitiveIndices
    //   47 = CullData（包围球 + 法线锥）
    static constexpr UINT kMeshletSrvSlot = 44; // 44..47，正好用到 kMaxTextures-1

    // M18：Mesh Shader 读顶点数据用的 SRV（StructuredBuffer<MeshVertex>，stride 32）。
    //   放在 48 是为了让 t0..t4 落在**连续的 44..48**，从而能用一张根描述符表绑定全部五个
    //   —— 描述符表只能取堆里的连续区间。为此 kMaxTextures 从 48 提到 56。
    static constexpr UINT kMeshletVertexSrvSlot = 48;

    // 渲染模式：CPU 逐实例提交 vs GPU-Driven ExecuteIndirect
    enum class RenderMode
    {
        CpuDriven,  // CPU 对每个可见实例发一次 SetCBV + DrawIndexedInstanced
        GpuDriven,  // CPU 只发一次 ExecuteIndirect；命令数量由 GPU 的计数器决定
        MeshShader, // M18：DispatchMesh，几何由 Mesh Shader 自己组装
    };
    static constexpr UINT kMaxInstances = 100000;  // 常量缓冲按此规模预分配
    static constexpr UINT kConstantStride = 256;   // 每个实例的常量槽位（CBV 256 字节对齐）
    static constexpr UINT kUiConstantSlot = kMaxInstances;        // UI 正交投影常量
    static constexpr UINT kDebugConstantSlot = kMaxInstances + 1;  // 调试线框（复用场景 viewProj）
    static constexpr UINT kGlobalConstantSlot = kMaxInstances + 2; // M12：GPU-Driven 的每帧全局常量
    static constexpr UINT kDepthVisualizeConstantSlot = kMaxInstances + 3; // M13：深度可视化常量
    static constexpr UINT kHZBVisualizeConstantSlot = kMaxInstances + 4;   // M14：HZB mip 可视化常量

    // 启动配置全部来自命令行；运行中可用按键改变（1/2/3 规模、C 剔除、V 可视化、方向键转视角）。
    bool Initialize(ID3D12Device* device, IDXGIFactory4* factory, HWND hwnd,
                    UINT width, UINT height, std::uint32_t initialInstanceCount = 1000,
                    bool useCpuCulling = true, float cameraYawDegrees = 0.0f,
                    int debugViewMode = 0, bool compareCullingAtStartup = false,
                    bool gpuDriven = false, bool depthPrepass = true,
                    bool depthVisualize = false, bool hzbVisualize = false,
                    std::uint32_t hzbMip = 0, bool occlusion = true,
                    bool occlusionViz = false, bool meshShaderSupported = false,
                    bool startInMeshShaderMode = false);
    void Shutdown();
    void Render();

    // 运行时开关：切换实例规模与剔除策略（由 Application 转发窗口按键）。
    void HandleKey(UINT virtualKey);

private:
    // --- 初始化分步 ---
    bool CreateCommandQueue(ID3D12Device* device);
    bool CreateSwapChain(IDXGIFactory4* factory);
    bool CreateRtvDescriptorHeap(ID3D12Device* device);
    bool CreateBackBufferRtvs(ID3D12Device* device);
    bool CreateCommandAllocators(ID3D12Device* device);
    bool CreateCommandList(ID3D12Device* device);
    bool CreateSyncObjects(ID3D12Device* device);
    bool CreateSrvDescriptorHeap(ID3D12Device* device);
    bool CreateRootSignature(ID3D12Device* device);
    bool CreatePipelineState(ID3D12Device* device);      // 网格 PSO
    bool CreateUiPipelineState(ID3D12Device* device);    // UI PSO（alpha 混合、关闭深度）
    bool CreateDebugPipelineState(ID3D12Device* device); // 调试线框 PSO（LINELIST、关闭深度）
    bool CreateDepthOnlyPipelineState(ID3D12Device* device);      // M13：Depth Prepass PSO
    bool CreateDepthVisualizePipelineState(ID3D12Device* device); // M13：深度可视化 PSO
    bool CreateHZBVisualizePipelineState(ID3D12Device* device);   // M14：HZB mip 可视化 PSO
    bool CreateDepthBuffer(ID3D12Device* device);
    bool CreateConstantBuffers(ID3D12Device* device);
    bool CreateAssets(ID3D12Device* device);

    // --- 场景 ---
    void RegenerateScene(std::uint32_t instanceCount);

    // --- 资源加载（加载文件 -> 上传显存）---
    bool LoadAndCreateMesh(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                           const char* relativePath, Mesh& outMesh,
                           std::vector<ComPtr<ID3D12Resource>>& stagingOut);
    bool LoadAndCreateTexture(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                              const char* relativePath, UINT srvIndex, Texture& outTexture,
                              std::vector<ComPtr<ID3D12Resource>>& stagingOut);

    // 等待当前 frame 上一轮提交的 GPU 工作完成（否则不能 Reset 它的 allocator）
    void WaitForGpu();

    // 手写 ResourceBarrier，显式表达资源状态转换（before -> after）
    void TransitionBackBuffer(ID3D12GraphicsCommandList* cmd, ID3D12Resource* backBuffer,
                              D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

    // 把统计文本写进 DebugText（每帧调用）
    void BuildStatisticsText();

    // ---- M9：GPU 常驻实例数据 ----
    // 把场景的全部 InstanceData 上传到 DEFAULT Heap 的 StructuredBuffer。
    // 走一次性的「临时命令列表 -> 提交 -> 等待」流程（场景切换时调用）。
    bool UploadInstanceData();

    // 在 GPU 上跑验证 CS 并把结果读回来比对（阻塞，仅按需调用）。
    void RunInstanceValidation();

    // ---- M10：GPU 视锥剔除 ----
    // 按 G 键触发：把 GPU 的 visibility buffer 读回来与 CPU 剔除结果逐实例对比。
    // （阻塞，仅按需调用；每帧的 dispatch 不需要回读。）
    void CompareGpuAndCpuCulling();

    // 用球坐标更新相机（yaw / pitch / distance）。
    // 之所以用球坐标而不是直接存 eye：绕场景中心旋转只需改一个角度，
    // 而且「角度 -> 位置」的映射是确定的，便于复现特定视角下的可见数。
    void UpdateCamera(double seconds);

    // 生成调试线框（视锥 + 包围球），供 DebugLines 渲染
    void BuildDebugVisualization(DirectX::FXMMATRIX viewProj,
                                 const std::vector<std::uint32_t>& visibleIndices);

    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    UINT m_rtvDescriptorSize = 0;
    ComPtr<ID3D12Resource> m_backBuffers[kFrameCount];
    FrameContext m_frames[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> m_commandList;

    // M18：DispatchMesh 声明在 ID3D12GraphicsCommandList6 上，基接口没有。
    // 命令列表本身早就实现了它，所以初始化时 QueryInterface 一次并缓存，
    // 避免每帧都做一次接口查询。
    ComPtr<ID3D12GraphicsCommandList6> m_commandList6;
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValue = 0; // 下一个要 Signal 的 Fence 值
    HANDLE m_fenceEvent = nullptr;
    UINT m_frameIndex = 0;   // 循环使用的 frame 索引（0..kFrameCount-1）

    HWND m_hwnd = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;

    // 高精度计时器
    double m_secondsPerCount = 0.0;
    std::int64_t m_startCounter = 0;

    // 管线状态
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;   // 网格
    ComPtr<ID3D12PipelineState> m_uiPipelineState; // UI 叠加

    // SRV 描述符堆：所有纹理（含字体图集）的 SRV 都放在这里
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    UINT m_srvDescriptorSize = 0;

    // 深度缓冲（DSV）
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    ComPtr<ID3D12Resource> m_depthBuffer; // D32_FLOAT 深度纹理

    // GPU 侧资源与材质
    Mesh m_stressMesh;     // 场景统一使用的网格（立方体，12 三角形）
    Texture m_stressTexture;
    Material m_stressMaterial;

    // 场景与可见性
    Scene m_scene;
    std::vector<std::uint32_t> m_visibleIndices; // 复用容器，避免每帧分配

    // ---- M9：GPU 常驻实例数据 ----
    // InstanceBuffer 把整个场景的 InstanceData 放在**显存**里，
    // 供后续里程碑的 Compute Shader 剔除 / 间接绘制读取。
    // M9 中主渲染路径仍然是 CPU-Driven（每个实例一次 SetCBV + Draw），
    // 这个缓冲只是「把数据准备好并验证正确」。
    InstanceBuffer m_instanceBuffer;
    InstanceValidator m_instanceValidator;
    ComPtr<ID3D12Device> m_device;      // 保存下来给场景切换时的重上传用
    bool m_validationRequested = false; // 按 T 键置位，在下一帧安全点执行

    // ---- M10/M11：GPU 视锥剔除 + Stream Compaction ----
    // VisibleInstanceList 保存**压缩后**的可见实例 ID 列表与其计数器。
    // 本阶段仍然**不消费**这份结果（渲染仍是 CPU-Driven，
    // ExecuteIndirect 属于下一个里程碑），它用于与 CPU 结果对比验证。
    VisibleInstanceList m_visibleList;
    GPUFrustumCuller m_frustumCuller;
    bool m_cullComparisonRequested = false; // 按 G 键置位
    bool m_compareCullingAtStartup = false; // --compare-cull：等到若干帧后再对比
    UINT m_frameCounter = 0;                // 已渲染帧数（用于延迟触发对比）
    // 上一帧剔除用的视锥平面（已归一化），供回读对比时 CPU 侧复用同一份数据
    DirectX::XMFLOAT4 m_lastFrustumPlanes[6] = {};

    // ---- M12：GPU-Driven Rendering（ExecuteIndirect）----
    // IndirectDrawCommands 持有间接参数缓冲、命令签名与命令生成 Compute Pass。
    IndirectDrawCommands m_indirectCommands;
    RenderMode m_renderMode = RenderMode::CpuDriven;

    // 全局常量（每帧一份，而不是每实例一份）：GPU-Driven 的 VS/PS 需要
    // viewProj 与光照方向，但不再有每实例的 ObjectConstants 可读。
    ComPtr<ID3D12RootSignature> m_gpuDrivenRootSignature;
    ComPtr<ID3D12PipelineState> m_gpuDrivenPipelineState;
    bool CreateGpuDrivenRootSignature(ID3D12Device* device);
    bool CreateGpuDrivenPipelineState(ID3D12Device* device);

    // ---- M18：Mesh Shader 渲染路径 ----
    //
    // 这是一条**独立**的路径，与上面两条并存：
    //   * 它有自己的根签名与 PSO（Mesh Shader 不能复用图形管线的 PSO）；
    //   * 它渲染的是**被 meshlet 化的那个网格**（sphere.obj），
    //     而 CPU-Driven / ExecuteIndirect 两条路径渲染的是实例化立方体场景。
    //
    // 为什么让它渲染同一个球体的「传统 indexed 版本」作为对照：
    //   验收要求「同一个 Mesh 可以通过 Mesh Shader 正确渲染」。
    //   只画一遍无法判断对错 —— 必须有一个用传统路径画出来的**同源参照物**，
    //   按 M 切换两条路径时画面应当**完全一致**，这才是可验证的「正确」。
    ComPtr<ID3D12RootSignature> m_meshShaderRootSignature;
    ComPtr<ID3D12PipelineState> m_meshShaderPipelineState;

    // sphere.obj 的 GPU 网格（顶点 + 索引）。
    // Mesh Shader 只用它的**顶点缓冲**（通过 SRV 读），索引缓冲留给传统路径对照渲染。
    Mesh m_meshletSphereMesh;
    std::uint32_t m_meshletSphereVertexCount = 0;

    // 能力标志：由 D3D12Context 查询到的 MeshShaderTier 决定。
    // 为 false 时 MeshShader 模式**不可进入**（M 键会跳过它）——
    // 这就是任务要求的 Feature Fallback。
    bool m_meshShaderSupported = false;

    bool CreateMeshShaderRootSignature(ID3D12Device* device);
    bool CreateMeshShaderPipelineState(ID3D12Device* device);
    void RecordMeshShaderDraw(const DirectX::XMMATRIX& viewProj);

    // ---- M19：Amplification Shader 的 meshlet 级剔除 ----
    //
    // 一个 AS 线程组处理 kAsGroupSize 个 meshlet（一线程一 meshlet）。
    // **必须与 shaders/MeshletAS.hlsl 里的 AS_GROUP_SIZE 保持一致** ——
    // 不一致会导致 CPU 发的组数与 AS 的假设错位，表现为部分 meshlet 从未被剔除测试。
    static constexpr std::uint32_t kAsGroupSize = 32;

    // AS 每帧写出的剔除统计（CPU 延迟若干帧读回，避免阻塞 GPU）
    struct MeshletCullStats
    {
        std::uint32_t frustumCulled = 0;    // 被视锥剔除的 meshlet 数
        std::uint32_t coneCulled = 0;       // 被法线锥剔除的 meshlet 数
        std::uint32_t visible = 0;          // 通过全部剔除的 meshlet 数
        std::uint32_t visibleTriangles = 0; // 这些 meshlet 包含的三角形总数
        std::uint32_t totalMeshlets = 0;    // 全场 meshlet 数（CPU 侧已知）
        std::uint32_t totalTriangles = 0;   // 全场三角形数（CPU 侧已知）
        // 每个 meshlet 的剔除状态：0 = 可见，1 = 视锥剔除，2 = 法线锥剔除。
        // 调试可视化用它给包围球着色 —— 只看统计数字无法判断「剔除的位置对不对」。
        std::vector<std::uint32_t> meshletStatus;
        bool valid = false;
    };
    MeshletCullStats m_meshletCullStats;

    // M19：两级剔除各自的开关，便于 A/B 验证「关掉某一级会怎样」
    bool m_meshletFrustumCullingEnabled = true;
    bool m_meshletConeCullingEnabled = true;

    // GPU 侧剔除统计缓冲：前 16 字节是 4 个计数器，之后是每 meshlet 的状态。
    ComPtr<ID3D12Resource> m_cullStatsBuffer;                // DEFAULT（UAV 写 / COPY 读写）
    ComPtr<ID3D12Resource> m_cullStatsReadback[kFrameCount]; // READBACK
    ComPtr<ID3D12Resource> m_cullStatsZero;                  // UPLOAD，全零，每帧拷贝清零
    D3D12_RESOURCE_STATES m_cullStatsState = D3D12_RESOURCE_STATE_COMMON;
    std::uint32_t m_cullStatsReadbackFrame[kFrameCount] = {};
    std::uint32_t m_cullStatsFrameCounter = 0;
    std::uint32_t m_cullStatsByteSize = 0;

    bool CreateMeshletCullStats(ID3D12Device* device, std::uint32_t meshletCount);
    void ReadbackMeshletCullStats();

    // 用 GPU-Driven 路径记录绘制（一次 ExecuteIndirect 提交全部可见实例）。
    // 用 GPU-Driven 路径记录绘制（一次 ExecuteIndirect 提交全部可见实例）。
    // visibleCount 是本帧 CPU 侧的可见数，只用作 MaxCommandCount 的**上限**；
    // 真正的条数由 GPU 的计数器决定（见实现里的两种取法）。
    void RecordGpuDrivenDraw(std::uint32_t visibleCount, UINT segmentSrvBaseSlot,
                             std::uint32_t lodCount);

    // ---- M13：Depth Prepass ----
    // 用可见实例的间接命令跑一遍深度（depth-only PSO，无 RTV）；
    // 之后 Main Pass 以 LESS_EQUAL + 不写深度的方式复用它。
    void RecordDepthPrepass();
    // 深度可视化：全屏三角形采样深度缓冲。
    void RecordDepthVisualization();

    // ---- M14：HZB ----
    // 构建完整的深度金字塔（mip 0 由深度生成，其余逐级 2x2 Max 降采样到 1x1）。
    void RecordHZBBuild();
    // 可视化指定的 HZB mip 级。
    void RecordHZBVisualization();

    // ---- M17：Meshlet 预处理（Microsoft DirectXMesh）----
    //
    // 这里只持有 CPU 侧结果；GPU 上传由 MeshletResources 负责。
    // 预处理发生在**加载期**（CreateAssets），不在每帧 ——
    // 聚类是 O(n log n) 级别的离线工作，放进帧循环毫无意义。
    MeshletBuilder m_meshletBuilder;
    MeshletResources m_meshletResources;

    // 每个 meshlet 的包围球按索引着色（M17 可视化）。J 键切换。
    bool m_meshletVisualizationEnabled = false;

    // ---- M16：GPU-Driven LOD ----
    // LOD 链元数据（CPU 生成，上传到 GPU 供剔除 CS 读取）
    std::vector<MeshLODRange> m_lodRanges;
    ComPtr<ID3D12Resource> m_lodMetadataBuffer;
    std::uint32_t m_lodCount = 1;
    bool m_lodEnabled = true;          // 按 L 切换
    float m_lodBias = 0.0f;            // 全局 LOD 偏置
    std::uint32_t m_lodTriangleBudget = 0; // 上一帧实际渲染的三角形数（统计）
    std::uint32_t m_lodCounts[8] = {};      // 每级实例数（延迟读回）
    uint64_t m_lodStatsReadbackFrame = 0;

    bool CreateLODChain(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                        std::vector<ComPtr<ID3D12Resource>>& stagingOut);
    void CreateLODSegmentSrvs(ID3D12Device* device, VisibleInstanceList& list);
    // 一次性验证：读回 per-LOD 计数器，确认选择真的在 GPU 上发生
    void VerifyLODDistribution();
    bool m_lodVerifyDone = false;

    // M16 可视化：读回每实例的 LOD（延迟，仅 debug viz 时执行）
    void ReadbackInstanceLOD();
    std::vector<std::uint32_t> m_instanceLODCache; // [实例下标] = LOD
    UINT m_lodReadbackCounter = 0;

    // ---- M15：HZB 遮挡剔除 ----
    // 对「视锥内候选」做保守遮挡测试，输出最终可见列表。
    void RecordOcclusionCulling(UINT frameIndexForStats);
    // 可视化：画被剔除实例的包围球（延迟一帧，避免同步）
    void BuildOcclusionDebugLines();

    HZBOcclusionCuller m_occlusionCuller;
    bool m_occlusionEnabled = true;      // 按 O 切换
    bool m_occlusionVisualize = false;   // 按 K 切换
    float m_occlusionDepthBias = 0.0002f; // NDC 单位的保守偏置

    // 遮挡剔除统计（延迟 kFrameRingSize 帧读回）
    HZBOcclusionCuller::FrameStats m_occlusionStats;

    // 遮挡剔除可视化：GPU 最终可见列表（延迟读回 + 排序，供双指针遍历）
    std::vector<std::uint32_t> m_occlusionDebugVisible;
    bool CreateOcclusionDebugReadback(ID3D12Device* device);
    ComPtr<ID3D12Resource> m_occlusionDebugReadback;

    HierarchicalZBuffer m_hzb;
    ComPtr<ID3D12PipelineState> m_hzbVisualizePipelineState;
    bool m_hzbEnabled = true;        // 按 H 切换构建
    bool m_hzbVisualizeEnabled = false; // 按 N 切换可视化
    UINT m_hzbViewMip = 0;           // 当前查看的 mip（[ / ] 切换）
    UINT m_avgHZBBuildUs = 0;        // 构建耗时（微秒，仅日志用）

    // 深度缓冲的状态转换（DEPTH_WRITE <-> PIXEL_SHADER_RESOURCE）
    void TransitionDepthBuffer(D3D12_RESOURCE_STATES newState);

    ComPtr<ID3D12PipelineState> m_depthOnlyPipelineState;
    ComPtr<ID3D12PipelineState> m_depthVisualizePipelineState;
    bool m_depthPrepassEnabled = true;    // 按 P 切换
    bool m_depthVisualizeEnabled = false; // 按 B 切换
    // Main Pass 的深度状态（WRITE_ZERO + LESS_EQUAL vs WRITE_ALL + LESS）
    // 是**烘焙进 PSO** 的，所以切换 prepass 时必须重建 PSO。
    // 重建要等 GPU 用完旧的 PSO，因此用标志延迟到下一帧的安全点执行。
    bool m_pipelineStateRebuildRequested = false;
    D3D12_RESOURCE_STATES m_depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    // 时间戳测量：剔除、Depth Pass、Main Pass 各自的 GPU 耗时
    GPUProfiler m_gpuProfiler;
    double m_avgGpuCullMs = 0.0;   // GPU 剔除 + 命令生成
    double m_avgGpuDepthMs = 0.0;  // Depth Prepass
    double m_avgGpuMainMs = 0.0;   // Main Pass
    float m_nearPlane = 0.5f;
    float m_farPlane = 500.0f;

    // ---- M8：CPU 视锥剔除 ----
    // 默认开启（M8 的主题就是只提交可见实例）；按 C 可切回「提交全部」，
    // 用来对比两种策略下的 draw 数量与 CPU 提交成本。
    bool m_cpuCullingEnabled = true;

    // 相机轨道参数（球坐标）。
    // 默认值与 M7 的固定视角 (0, 10, -48) 完全一致（距离 49、俯仰 11.8°、yaw 0），
    // 因此 M7 采集的 benchmark 数据在 M8 依然可以直接对比。
    // 相机**位置固定**在 (0, 10, -48)（与 M7 的基准视角一致，因此 benchmark 可比），
    // yaw / pitch 只改变朝向。默认 pitch = asin(-10 / 49.03) ≈ -11.769°，正好看向原点。
    float m_cameraYawDegrees = 0.0f;
    float m_cameraPitchDegrees = -11.769f;
    bool m_autoOrbit = false;     // 默认静止：benchmark 需要可复现的视角
    float m_orbitDirection = 1.0f;

    // 调试可视化：0 = 关闭，1 = 只画视锥，2 = 视锥 + 包围球
    DebugLines m_debugLines;
    ComPtr<ID3D12PipelineState> m_debugPipelineState;
    int m_debugViewMode = 0;

    // 统计（指数滑动平均，避免数字剧烈跳动看不清）
    FrameStats m_stats;
    double m_avgCpuFrameMs = 0.0;
    double m_avgCullMs = 0.0;
    double m_avgUpdateMs = 0.0;
    double m_avgRecordMs = 0.0;
    double m_avgPresentMs = 0.0;

    // 屏幕叠加文本（替代不可用的 ImGui）
    DebugText m_debugText;

    Camera m_camera;

    // ---- M14 验证：读回 HZB 各级的采样值，检查 Max reduction 的单调性 ----
    // 这是一次性调试读回，**不参与**金字塔的生成流程（生成全在 GPU 上）。
    bool CreateHZBVerifyReadback(ID3D12Device* device);
    void VerifyHZBLevels();

    ComPtr<ID3D12Resource> m_hzbVerifyReadback;
    UINT m_hzbVerifyRowPitch = 0;
    bool m_hzbVerifyDone = false;
};
