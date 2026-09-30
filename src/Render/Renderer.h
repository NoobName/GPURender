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
    static constexpr UINT kMaxTextures = 16;       // SRV 描述符堆容量
    static constexpr UINT kMaxInstances = 100000;  // 常量缓冲按此规模预分配
    static constexpr UINT kConstantStride = 256;   // 每个实例的常量槽位（CBV 256 字节对齐）
    static constexpr UINT kUiConstantSlot = kMaxInstances;        // UI 正交投影常量
    static constexpr UINT kDebugConstantSlot = kMaxInstances + 1; // 调试线框（复用场景 viewProj）

    // 启动配置全部来自命令行；运行中可用按键改变（1/2/3 规模、C 剔除、V 可视化、方向键转视角）。
    bool Initialize(ID3D12Device* device, IDXGIFactory4* factory, HWND hwnd,
                    UINT width, UINT height, std::uint32_t initialInstanceCount = 1000,
                    bool useCpuCulling = true, float cameraYawDegrees = 0.0f,
                    int debugViewMode = 0);
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
};
