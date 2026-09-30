#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "Render/Camera.h"
#include "Render/GPUBuffer.h"
#include "Render/Material.h"
#include "Render/Mesh.h"
#include "Render/Texture.h"

using Microsoft::WRL::ComPtr;

// 每个可绘制对象每帧需要一份常量数据（主要是各自的 model 矩阵）。
//
// 布局（144 字节，实际按 256 字节对齐占位）：
//   offset  0 : model          float4x4 -> 模型 -> 世界
//   offset 64 : viewProj       float4x4 -> 世界 -> 裁剪
//   offset 128: lightDirection float4   -> 世界空间光照方向
//
// 之所以把 viewProj 也放进「每对象」常量里（而不是单独一份全局常量）：
// M6 的对象数量很少，这样每个 draw 只需要绑定一次 CBV，代码最简单直接。
// 真正需要优化时再拆成「每帧常量 + 每对象常量」两份。
struct ObjectConstants
{
    DirectX::XMFLOAT4X4 model;
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT4 lightDirection;
};

// 每帧独立的资源与同步状态。
//
// 为什么需要这个结构：CommandAllocator 是「记录命令的内存池」，只要 GPU 还在
// 执行它里面记录的命令，就不能 Reset（否则会破坏正在被 GPU 读取的命令内存）。
// 因此给每个 frame 一份独立的 allocator，并在重用某份之前通过 Fence 确认 GPU
// 已经完成了它上一轮的工作。fenceValue 记录该帧提交后对应的 Fence 值，供下一轮
// 等待时查询。
struct FrameContext
{
    ComPtr<ID3D12CommandAllocator> commandAllocator; // 该帧独占的命令分配器
    UINT64 fenceValue = 0;                           // 该帧提交后对应的 Fence 值

    // 该帧独占的常量缓冲（Upload Heap）。内部按 256 字节切成 kMaxRenderItems 个槽位，
    // 第 i 个对象写在第 i * 256 字节处。CPU 写完、GPU 读，靠帧间 Fence 保证不会
    // 在 GPU 还在读的时候被下一帧覆盖。
    GPUBuffer constantBuffer;
};

// 一个待绘制对象：Mesh + Material + Transform。
//
// 这三者就是 M6 要建立的「可复用渲染路径」：
//   - 同一个 Mesh 可以被多个 RenderItem 引用；
//   - 同一个 Material 也可以被多个 RenderItem 引用；
//   - 每个 item 的差异只体现在各自的 model 矩阵上。
// 渲染器只知道遍历这个列表，不关心具体是什么模型 —— 这就是从
// 「硬编码一个立方体」走向「资源驱动渲染」的分界线。
struct RenderItem
{
    const Mesh* mesh = nullptr;
    const Material* material = nullptr;
    DirectX::XMFLOAT3 worldPosition = { 0.0f, 0.0f, 0.0f };
    float scale = 1.0f;
    float rotationSpeed = 1.0f; // 绕 Y 轴自转，弧度/秒
};

// 负责 Command Submission、Presentation 与网格绘制的渲染器。
//
// M6 的组成：在 M5（CommandQueue + SwapChain + RTV/DSV + Fence + RootSignature + PSO）
// 的基础上，新增 GPU 侧的 Mesh / Texture / Material 资源，以及
// SRV 描述符堆、静态采样器、UV 与纹理采样。
class Renderer
{
public:
    static constexpr UINT kFrameCount = 3;    // 三重缓冲
    static constexpr UINT kMaxRenderItems = 8; // 常量缓冲按此数量预留槽位
    static constexpr UINT kMaxTextures = 16;   // SRV 描述符堆容量

    bool Initialize(ID3D12Device* device, IDXGIFactory4* factory, HWND hwnd, UINT width, UINT height);
    void Shutdown();
    void Render(); // 一帧：等待 -> 写常量 -> 记录命令 -> 提交 -> Present -> Signal

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
    bool CreatePipelineState(ID3D12Device* device);
    bool CreateDepthBuffer(ID3D12Device* device);
    bool CreateConstantBuffers(ID3D12Device* device);
    bool CreateAssets(ID3D12Device* device);
    bool CreateRenderItems();

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

    // 高精度计时器（用于自转动画）
    double m_secondsPerCount = 0.0;
    std::int64_t m_startCounter = 0;

    // 管线状态
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;

    // SRV 描述符堆：所有纹理的 SRV 都放在这里，绘制时用「索引」定位。
    // 必须是 SHADER_VISIBLE，否则着色器读不到。
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    UINT m_srvDescriptorSize = 0;

    // 深度缓冲（DSV）
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    ComPtr<ID3D12Resource> m_depthBuffer; // D32_FLOAT 深度纹理

    // GPU 侧资源与材质
    Mesh m_sphereMesh;
    Mesh m_cubeMesh;
    Texture m_checkerTexture;
    Texture m_uvGridTexture;
    Material m_checkerMaterial;
    Material m_uvGridMaterial;

    // 待绘制对象列表（Mesh + Material + Transform）
    std::vector<RenderItem> m_renderItems;

    // 相机
    Camera m_camera;
};
