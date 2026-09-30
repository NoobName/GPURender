#include "Render/Renderer.h"

#include "Asset/ObjLoader.h"
#include "Asset/TgaLoader.h"
#include "Render/ShaderCompiler.h"
#include "Render/UploadHelper.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace DirectX;

namespace
{
// D3D12 常量缓冲（CBV）绑定要求起始地址按 256 字节对齐，
// 所以每个对象的常量数据都按 256 字节占一个槽位。
constexpr UINT Align256(UINT size)
{
    return (size + 255u) & ~255u;
}

// 实例常量槽位大小由 Renderer::kConstantStride 定义（256 字节，CBV 对齐要求）。

// 取可执行文件所在目录。
// 构建时 CMake 会把 shaders/ 与 assets/ 拷贝到该目录旁边，
// 因此运行期用「exe 目录 + 相对路径」即可定位资源，不依赖当前工作目录。
std::string GetExecutableDirectory()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0)
    {
        return std::string();
    }

    const std::wstring fullPath(path, length);
    const std::size_t separator = fullPath.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
    {
        return std::string();
    }
    const std::wstring directory = fullPath.substr(0, separator + 1);

    // 宽字符 -> UTF-8，供 std::ifstream 使用。
    const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, directory.c_str(),
                                               static_cast<int>(directory.size()),
                                               nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0)
    {
        return std::string();
    }
    std::string result(static_cast<std::size_t>(sizeNeeded), '\0');
    WideCharToMultiByte(CP_UTF8, 0, directory.c_str(), static_cast<int>(directory.size()),
                        result.data(), sizeNeeded, nullptr, nullptr);
    return result;
}
} // namespace

bool Renderer::Initialize(ID3D12Device* device, IDXGIFactory4* factory, HWND hwnd,
                          UINT width, UINT height, std::uint32_t initialInstanceCount,
                          bool useCpuCulling, float cameraYawDegrees, int debugViewMode)
{
    m_cpuCullingEnabled = useCpuCulling;
    m_cameraYawDegrees = cameraYawDegrees;
    m_debugViewMode = debugViewMode;

    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

    // 初始化高精度计时器（用于旋转动画）
    LARGE_INTEGER freq = {};
    QueryPerformanceFrequency(&freq);
    m_secondsPerCount = 1.0 / static_cast<double>(freq.QuadPart);
    LARGE_INTEGER start = {};
    QueryPerformanceCounter(&start);
    m_startCounter = start.QuadPart;

    if (!CreateCommandQueue(device)) return false;
    if (!CreateSwapChain(factory)) return false;
    if (!CreateRtvDescriptorHeap(device)) return false;
    if (!CreateBackBufferRtvs(device)) return false;
    if (!CreateCommandAllocators(device)) return false;
    if (!CreateCommandList(device)) return false;
    if (!CreateSyncObjects(device)) return false;
    if (!CreateSrvDescriptorHeap(device)) return false; // 必须先于 CreateAssets（要往里写 SRV）
    if (!CreateRootSignature(device)) return false;
    if (!CreatePipelineState(device)) return false;      // 网格 PSO
    if (!CreateUiPipelineState(device)) return false;    // UI 叠加 PSO
    if (!CreateDebugPipelineState(device)) return false; // 调试线框 PSO
    if (!CreateDepthBuffer(device)) return false;
    if (!CreateConstantBuffers(device)) return false;
    if (!CreateAssets(device)) return false;
    if (!m_debugLines.Initialize(device)) return false;

    // 相机用球坐标 (yaw, pitch, distance) 描述，绕场景中心旋转。
    //
    // 默认值与 M7 的固定视角完全一致（距离 49、俯仰 11.8°、yaw 0 ⇒ (0, 10, -48)），
    // 因此 M7 采集的 benchmark 数据在 M8 依然可比。
    //
    // 相机刻意放在**场景球体内部**（场景半径 55）：身后与视锥外的大量实例
    // 会被真正剔除，Visible Count 才有意义；若把相机远远放在球外，
    // 整个场景都落在视锥里，可见率恒为 100%，剔除就无从验证。
    m_camera.SetPerspective(60.0f, static_cast<float>(m_width) / static_cast<float>(m_height),
                            0.5f, 500.0f);
    UpdateCamera(0.0);

    // 初始场景规模来自命令行（默认 1000），运行中按 1 / 2 / 3 可切换
    RegenerateScene(initialInstanceCount);

    return true;
}

void Renderer::Shutdown()
{
    // 关闭前必须等待 GPU 完成所有已提交的工作。
    if (m_fence != nullptr && m_fenceValue > 0)
    {
        if (m_fence->GetCompletedValue() < m_fenceValue)
        {
            m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

    if (m_fenceEvent != nullptr)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    // 其余 ComPtr 随成员析构自动释放。
}

void Renderer::Render()
{
    // 计时点：0 = 帧开始，cullEnd = 剔除结束，1 = 数据准备完毕，2 = 记录完毕，3 = Present 返回
    LARGE_INTEGER counter0 = {};
    LARGE_INTEGER counterCullEnd = {};
    LARGE_INTEGER counter1 = {};
    LARGE_INTEGER counter2 = {};
    LARGE_INTEGER counter3 = {};
    QueryPerformanceCounter(&counter0);

    // 1. 等待当前 frame 上一轮的 GPU 工作完成（可安全复用其 allocator 与常量缓冲）。
    WaitForGpu();

    const UINT backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
    FrameContext& frame = m_frames[m_frameIndex];

    // 2. 相机：由球坐标更新（支持运行时旋转与自动环绕）。
    LARGE_INTEGER nowCounter = {};
    QueryPerformanceCounter(&nowCounter);
    const double seconds =
        static_cast<double>(nowCounter.QuadPart - m_startCounter) * m_secondsPerCount;
    UpdateCamera(seconds);

    const XMMATRIX view = m_camera.GetView();
    const XMMATRIX proj = m_camera.GetProjection();
    const XMMATRIX viewProj = XMMatrixMultiply(view, proj);

    XMFLOAT4 lightDirection = {};
    XMStoreFloat4(&lightDirection,
                 XMVector3Normalize(XMVectorSet(-0.45f, 0.75f, -0.48f, 0.0f)));

    // 3. CPU 视锥剔除：提取 6 个裁剪平面，逐个测试实例的世界空间包围球。
    //    这是 M8 的主题，因此**单独计时**，用来量化「剔除本身的成本」。
    const std::vector<InstanceData>& instances = m_scene.GetInstances();
    LARGE_INTEGER counterCullBegin = {};
    QueryPerformanceCounter(&counterCullBegin);
    CullInstancesByFrustum(instances, viewProj, m_visibleIndices);
    QueryPerformanceCounter(&counterCullEnd);

    const std::uint32_t totalCount = static_cast<std::uint32_t>(instances.size());
    const std::uint32_t visibleCount = static_cast<std::uint32_t>(m_visibleIndices.size());

    // 关闭剔除时提交全部实例 —— 用来对比「剔除省下的提交成本」与「剔除本身的成本」。
    const std::uint32_t submitCount = m_cpuCullingEnabled ? visibleCount : totalCount;

    // 4. 先生成统计文本。
    //    必须在写 UI 常量之前调用：Begin() 会记录本帧的屏幕尺寸，
    //    而 UI 的正交投影矩阵正是由这个尺寸算出来的（否则宽高为 0，矩阵退化）。
    BuildStatisticsText();

    // 5. 把本帧要提交的实例常量写进常量缓冲（第 i 个实例位于 i * 256 字节处）。
    //    一次性 Map 整块、写完再 Unmap —— 100k 实例时这里是 24 MB 的写入，
    //    逐实例 Map/Unmap 的开销会盖过真正想测的提交开销。
    D3D12_RANGE readRange = { 0, 0 }; // CPU 只写不读
    void* mapped = frame.constantBuffer.Map(0, &readRange);
    if (mapped == nullptr)
    {
        return;
    }

    std::uint8_t* const constantBase = static_cast<std::uint8_t*>(mapped);
    for (std::uint32_t i = 0; i < submitCount; ++i)
    {
        const std::uint32_t instanceIndex = m_cpuCullingEnabled ? m_visibleIndices[i] : i;

        InstanceConstants constants = {};
        // 矩阵约定：DirectXMath 是 row-major，HLSL 的 float4x4 默认 column-major，
        // 两者组合后 HLSL 读到的正好是转置，配合 mul(matrix, vector) 即为正确变换。
        constants.model = instances[instanceIndex].world;
        XMStoreFloat4x4(&constants.viewProj, viewProj);
        constants.lightDirection = lightDirection;

        std::memcpy(constantBase + static_cast<std::size_t>(i) * kConstantStride,
                    &constants, sizeof(constants));
    }

    // UI 叠加层槽位：屏幕空间正交投影矩阵。
    {
        InstanceConstants uiConstants = {};
        uiConstants.viewProj = m_debugText.GetOrthoMatrix();
        std::memcpy(constantBase + static_cast<std::size_t>(kUiConstantSlot) * kConstantStride,
                    &uiConstants, sizeof(uiConstants));
    }

    // 调试线框槽位：复用场景的 viewProj（线框顶点本身已经是世界空间坐标）。
    {
        InstanceConstants debugConstants = {};
        XMStoreFloat4x4(&debugConstants.viewProj, viewProj);
        std::memcpy(constantBase + static_cast<std::size_t>(kDebugConstantSlot) * kConstantStride,
                    &debugConstants, sizeof(debugConstants));
    }

    frame.constantBuffer.Unmap(0, nullptr);

    // 6. 生成调试线框与统计文本的顶点，并上传到 GPU（属于 CPU 侧的 update 阶段）。
    if (m_debugViewMode > 0)
    {
        BuildDebugVisualization(viewProj, m_visibleIndices);
    }
    else
    {
        m_debugLines.Begin(); // 关闭可视化时清空，避免上一帧的线框残留
    }
    m_debugLines.End();
    m_debugText.End();

    QueryPerformanceCounter(&counter1);

    // 6. 复用 allocator 与 command list。
    frame.commandAllocator->Reset();
    m_commandList->Reset(frame.commandAllocator.Get(), nullptr);

    ID3D12Resource* backBuffer = m_backBuffers[backBufferIndex].Get();

    // 7. 状态转换 PRESENT -> RENDER_TARGET（back buffer 由显示交给渲染）。
    TransitionBackBuffer(m_commandList.Get(), backBuffer,
                         D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    // 8. 视口 / 裁剪矩形 / 渲染目标绑定。
    const D3D12_VIEWPORT viewport = {
        0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f
    };
    const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissor);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(backBufferIndex) * m_rtvDescriptorSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    // 9. 清屏（颜色 + 深度）。
    const float clearColor[4] = { 0.02f, 0.02f, 0.05f, 1.0f };
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // 10. 设置管线状态。根签名 / 描述符堆整帧只设一次 ——
    //     这正是要凸显的结构：昂贵的状态放循环外，廉价的绑定放循环内。
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->SetPipelineState(m_pipelineState.Get());

    ID3D12DescriptorHeap* const descriptorHeaps[] = { m_srvHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, descriptorHeaps);

    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 11. **baseline 的核心**：每个实例一次 SetGraphicsRootConstantBufferView + 一次 Draw。
    //     这里刻意没有任何剔除、批处理或间接绘制 —— 就是要如实测出「朴素 CPU 提交」有多贵。
    const D3D12_VERTEX_BUFFER_VIEW& vertexBufferView = m_stressMesh.GetVertexBufferView();
    const D3D12_INDEX_BUFFER_VIEW& indexBufferView = m_stressMesh.GetIndexBufferView();
    m_commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
    m_commandList->IASetIndexBuffer(&indexBufferView);

    D3D12_GPU_DESCRIPTOR_HANDLE materialSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    materialSrv.ptr += static_cast<UINT64>(m_stressMaterial.srvIndex) * m_srvDescriptorSize;
    m_commandList->SetGraphicsRootDescriptorTable(1, materialSrv);

    const UINT indexCount = m_stressMesh.GetIndexCount();
    const D3D12_GPU_VIRTUAL_ADDRESS constantBaseAddress = frame.constantBuffer.GetGPUVirtualAddress();

    for (std::uint32_t i = 0; i < submitCount; ++i)
    {
        m_commandList->SetGraphicsRootConstantBufferView(
            0, constantBaseAddress + static_cast<UINT64>(i) * kConstantStride);
        m_commandList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
    }

    // 12. 调试线框（视锥 / 包围球）：换 PSO（LINELIST 拓扑、关闭深度测试），其余状态复用。
    if (m_debugLines.HasContent())
    {
        m_commandList->SetPipelineState(m_debugPipelineState.Get());
        m_commandList->SetGraphicsRootConstantBufferView(
            0, constantBaseAddress + static_cast<UINT64>(kDebugConstantSlot) * kConstantStride);
        m_debugLines.Render(m_commandList.Get());

        // Render() 把拓扑改成了 LINELIST，后面 UI 用的是三角形，必须改回来
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    // 13. UI 叠加：切换 PSO（alpha 混合、关闭深度），其余状态复用。
    if (m_debugText.HasContent())
    {
        m_commandList->SetPipelineState(m_uiPipelineState.Get());

        D3D12_GPU_DESCRIPTOR_HANDLE fontSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        fontSrv.ptr += static_cast<UINT64>(m_debugText.GetSrvIndex()) * m_srvDescriptorSize;
        m_commandList->SetGraphicsRootDescriptorTable(1, fontSrv);

        m_commandList->SetGraphicsRootConstantBufferView(
            0, constantBaseAddress + static_cast<UINT64>(kUiConstantSlot) * kConstantStride);
        m_debugText.Render(m_commandList.Get());
    }

    // 13. 状态转换 RENDER_TARGET -> PRESENT（渲染完交回显示引擎）。
    TransitionBackBuffer(m_commandList.Get(), backBuffer,
                         D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    // 14. 关闭命令列表并提交，Present，Signal。
    m_commandList->Close();
    QueryPerformanceCounter(&counter2);

    ID3D12CommandList* const lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    m_swapChain->Present(1, 0);
    QueryPerformanceCounter(&counter3);

    ++m_fenceValue;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValue);
    frame.fenceValue = m_fenceValue;

    m_frameIndex = (m_frameIndex + 1) % kFrameCount;

    // 16. 统计：换算成毫秒，并做指数滑动平均（原始值每帧跳动太大，看不清趋势）。
    //
    //     注意 cullMs 单独用 counterCullBegin/End 计量，不能拿 counter1 - counter0：
    //     后者还包含了 WaitForGpu（等上一帧 GPU 完成）与相机更新的时间。
    const double toMilliseconds = 1000.0 * m_secondsPerCount;
    m_stats.cullMs =
        static_cast<double>(counterCullEnd.QuadPart - counterCullBegin.QuadPart) * toMilliseconds;
    m_stats.updateMs =
        static_cast<double>(counter1.QuadPart - counterCullEnd.QuadPart) * toMilliseconds;
    m_stats.recordMs = static_cast<double>(counter2.QuadPart - counter1.QuadPart) * toMilliseconds;
    m_stats.presentMs = static_cast<double>(counter3.QuadPart - counter2.QuadPart) * toMilliseconds;
    m_stats.cpuFrameMs = static_cast<double>(counter3.QuadPart - counter0.QuadPart) * toMilliseconds;
    m_stats.submittedDrawCalls = submitCount;
    m_stats.totalInstances = totalCount;
    m_stats.visibleInstances = visibleCount;
    m_stats.culledInstances = totalCount - visibleCount;

    constexpr double kSmoothing = 0.1;
    m_avgCullMs = m_avgCullMs * (1.0 - kSmoothing) + m_stats.cullMs * kSmoothing;
    m_avgUpdateMs = m_avgUpdateMs * (1.0 - kSmoothing) + m_stats.updateMs * kSmoothing;
    m_avgRecordMs = m_avgRecordMs * (1.0 - kSmoothing) + m_stats.recordMs * kSmoothing;
    m_avgPresentMs = m_avgPresentMs * (1.0 - kSmoothing) + m_stats.presentMs * kSmoothing;
    m_avgCpuFrameMs = m_avgCpuFrameMs * (1.0 - kSmoothing) + m_stats.cpuFrameMs * kSmoothing;

    // 每 60 帧往控制台打一行：方便脚本采集，也方便与后续 GPU-Driven 版本逐项对比。
    static UINT benchmarkFrameCounter = 0;
    if ((++benchmarkFrameCounter % 60u) == 0u)
    {
        std::cout << "[Bench] instances=" << m_stats.totalInstances
                  << " visible=" << m_stats.visibleInstances
                  << " culled=" << m_stats.culledInstances
                  << " draws=" << m_stats.submittedDrawCalls
                  << " culling=" << (m_cpuCullingEnabled ? "on" : "off")
                  << " cpuFrame=" << m_avgCpuFrameMs << "ms"
                  << " cull=" << m_avgCullMs << "ms"
                  << " update=" << m_avgUpdateMs << "ms"
                  << " record=" << m_avgRecordMs << "ms"
                  << " present=" << m_avgPresentMs << "ms\n";
    }
}

bool Renderer::CreateCommandQueue(ID3D12Device* device)
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;
    return SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_commandQueue)));
}

bool Renderer::CreateSwapChain(IDXGIFactory4* factory)
{
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = m_width;
    desc.Height = m_height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kFrameCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags = 0;

    ComPtr<IDXGISwapChain1> swapChain1;
    if (FAILED(factory->CreateSwapChainForHwnd(
            m_commandQueue.Get(), m_hwnd, &desc, nullptr, nullptr, &swapChain1)))
    {
        return false;
    }
    return SUCCEEDED(swapChain1.As(&m_swapChain));
}

bool Renderer::CreateRtvDescriptorHeap(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    desc.NumDescriptors = kFrameCount;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    desc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_rtvHeap))))
    {
        return false;
    }
    m_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

bool Renderer::CreateBackBufferRtvs(ID3D12Device* device)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i)
    {
        if (FAILED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i]))))
        {
            return false;
        }
        device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }
    return true;
}

bool Renderer::CreateCommandAllocators(ID3D12Device* device)
{
    for (UINT i = 0; i < kFrameCount; ++i)
    {
        if (FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_frames[i].commandAllocator))))
        {
            return false;
        }
        m_frames[i].fenceValue = 0;
    }
    return true;
}

bool Renderer::CreateCommandList(ID3D12Device* device)
{
    if (FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_frames[0].commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList))))
    {
        return false;
    }
    return SUCCEEDED(m_commandList->Close());
}

bool Renderer::CreateSyncObjects(ID3D12Device* device)
{
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))))
    {
        return false;
    }
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return m_fenceEvent != nullptr;
}

void Renderer::WaitForGpu()
{
    const UINT64 fenceValue = m_frames[m_frameIndex].fenceValue;
    if (fenceValue == 0)
    {
        return;
    }
    if (m_fence->GetCompletedValue() < fenceValue)
    {
        m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void Renderer::TransitionBackBuffer(ID3D12GraphicsCommandList* cmd, ID3D12Resource* backBuffer,
                                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);
}

bool Renderer::CreateRootSignature(ID3D12Device* device)
{
    // Root Signature 是「着色器资源绑定的接口」：它规定着色器能看到哪些资源，
    // 以及这些资源通过哪种方式绑定（根常量 / 根描述符 / 描述符表）。
    //
    // M6 的布局：
    //   根参数 0：b0 上的 CBV      —— 每对象的 ObjectConstants
    //   根参数 1：t0 上的 SRV 表   —— 基础颜色纹理
    //   静态采样器：s0             —— 线性过滤 + Wrap 寻址
    D3D12_ROOT_PARAMETER rootParams[2] = {};

    // --- 根参数 0：常量缓冲 CBV ---
    // 用「根 CBV」而不是描述符表：常量缓冲每个对象都要换，
    // 根描述符直接内联在命令流里，绑定最省事也最快。
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0; // b0
    rootParams[0].Descriptor.RegisterSpace = 0;
    // 顶点着色器读 model / viewProj，像素着色器读 lightDirection，因此两边都可见。
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // --- 根参数 1：SRV 描述符表 ---
    // 纹理走描述符表：这样同一个 PSO 可以通过换表里的描述符来切换纹理，
    // 不必为每张纹理各建一个 Root Signature。
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0; // t0
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // --- 静态采样器 s0 ---
    // 采样器描述的是「怎么采样」：过滤方式（点/线性/各向异性）与寻址模式（wrap/clamp/mirror）。
    // 放进 Root Signature 作为静态采样器，PSO 创建时就固定下来，运行时无需再绑定；
    // 代价是不能逐 draw 更换（M6 用不到，将来需要时改用采样器堆）。
    D3D12_STATIC_SAMPLER_DESC staticSampler = {};
    staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.MipLODBias = 0.0f;
    staticSampler.MaxAnisotropy = 1;
    staticSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    staticSampler.MinLOD = 0.0f;
    staticSampler.MaxLOD = D3D12_FLOAT32_MAX;
    staticSampler.ShaderRegister = 0; // s0
    staticSampler.RegisterSpace = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = rootParams;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &staticSampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &signature, &error)))
    {
        std::cerr << "[Renderer] D3D12SerializeRootSignature failed\n";
        return false;
    }

    return SUCCEEDED(device->CreateRootSignature(0,
        signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)));
}

bool Renderer::CreatePipelineState(ID3D12Device* device)
{
    // 1. 用 DXC 编译 shader。
    std::string errorMsg;
    std::vector<std::uint8_t> vsBytecode = ShaderCompiler::Compile(
        L"MeshVS.hlsl", L"main", L"vs_6_0", errorMsg);
    if (vsBytecode.empty())
    {
        std::cerr << "[Renderer] Vertex shader compile failed:\n" << errorMsg << "\n";
        return false;
    }
    std::vector<std::uint8_t> psBytecode = ShaderCompiler::Compile(
        L"MeshPS.hlsl", L"main", L"ps_6_0", errorMsg);
    if (psBytecode.empty())
    {
        std::cerr << "[Renderer] Pixel shader compile failed:\n" << errorMsg << "\n";
        return false;
    }

    // 2. Input Layout：告诉 IA 阶段「顶点缓冲里的字节如何对应到着色器的语义」。
    //    偏移 0 / 12 / 24 与 MeshVertex 的 32 字节布局严格一致
    //    （该结构体有 static_assert 兜底，改坏了编译期就会报错）。
    const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // 3. PSO。
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_rootSignature.Get();
    desc.VS = { vsBytecode.data(), vsBytecode.size() };
    desc.PS = { psBytecode.data(), psBytecode.size() };
    // Blend：不混合
    desc.BlendState.AlphaToCoverageEnable = FALSE;
    desc.BlendState.IndependentBlendEnable = FALSE;
    desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    // Rasterizer：实心填充，不剔除背面（M5 简化，深度测试保证正确遮挡）
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    // 深度模板：开启深度测试（写入全部、比较 LESS）
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    // 输入布局与图元类型
    desc.InputLayout = { inputLayout, static_cast<UINT>(sizeof(inputLayout) / sizeof(inputLayout[0])) };
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    // 渲染目标格式、深度格式、采样
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;

    return SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_pipelineState)));
}

bool Renderer::CreateSrvDescriptorHeap(ID3D12Device* device)
{
    // SRV 描述符堆：所有纹理的 SRV 都集中放在这里，绘制时用「索引」定位。
    //
    // 为什么必须显式建堆：
    //   着色器不能直接持有 ID3D12Resource —— 它只能通过「描述符」访问资源。
    //   而描述符必须存放在描述符堆里；要让着色器能读到，堆必须带
    //   D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE 标志。
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; // CBV/SRV/UAV 共用同一种堆类型
    heapDesc.NumDescriptors = kMaxTextures;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDesc.NodeMask = 0;

    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap))))
    {
        std::cerr << "[Renderer] Failed to create SRV descriptor heap.\n";
        return false;
    }

    // 描述符大小由设备决定，不能写死：不同厂商/架构可能不同（CBV_SRV_UAV 常见为 32 字节）。
    m_srvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}



bool Renderer::CreateDepthBuffer(ID3D12Device* device)
{
    // 1. DSV Descriptor Heap（1 个深度模板视图）。
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dsvHeapDesc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap))))
    {
        return false;
    }

    // 2. 深度缓冲资源（D32_FLOAT 纹理，Default Heap，初始 DEPTH_WRITE）。
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = m_width;
    depthDesc.Height = m_height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // 提供清屏值（ClearDepthStencilView 会用，也便于驱动优化）
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    if (FAILED(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(&m_depthBuffer))))
    {
        return false;
    }

    // 3. DSV：描述如何访问深度缓冲。
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    device->CreateDepthStencilView(m_depthBuffer.Get(), &dsvDesc,
                                   m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    return true;
}

bool Renderer::CreateConstantBuffers(ID3D12Device* device)
{
    // 每帧一份实例常量缓冲，按「最大实例数 + 1（UI 槽位）」预分配。
    //
    // 为什么每帧一份：CPU 写第 N 帧的常量时，GPU 可能仍在读第 N-1 / N-2 帧的同一块内存。
    // 「每帧独立 + 帧间 Fence 等待」保证不会写到 GPU 正在读的数据上。
    //
    // 为什么按最大值预分配而不随规模重建：100000 * 256 B ≈ 24.4 MB/帧、三帧约 73 MB，
    // 现代机器完全可以接受；换来的是运行时切 1k / 10k / 100k 时零重建、零卡顿，
    // 测量结果也更干净（不会把资源创建时间混进去）。
    // 槽位：0..kMaxInstances-1 = 实例，kUiConstantSlot = UI，kDebugConstantSlot = 调试线框
    const std::uint64_t bufferSize =
        static_cast<std::uint64_t>(kDebugConstantSlot + 1) * kConstantStride;

    for (UINT i = 0; i < kFrameCount; ++i)
    {
        if (!m_frames[i].constantBuffer.Initialize(device, bufferSize,
                D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ))
        {
            std::cerr << "[Renderer] Failed to create instance constant buffer.\n";
            return false;
        }
    }
    return true;
}

bool Renderer::CreateAssets(ID3D12Device* device)
{
    // 资源上传用的是一次性的「临时命令列表 -> 提交 -> 等待」流程：
    // 初始化阶段不在乎这点延迟，换来的是代码直白、同步关系明确。
    ComPtr<ID3D12CommandAllocator> uploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> uploadList;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&uploadAllocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         uploadAllocator.Get(), nullptr,
                                         IID_PPV_ARGS(&uploadList))))
    {
        std::cerr << "[Renderer] Failed to create upload command list.\n";
        return false;
    }

    // staging（Upload Heap）必须活到 GPU 执行完这批上传命令之后，
    // 所以先收集起来，等 fence 等待完成再统一释放。
    std::vector<ComPtr<ID3D12Resource>> stagingResources;

    // 场景统一使用的网格：立方体（12 个三角形）。
    // 刻意用低模 —— M7 测量的是 **CPU 提交开销**，必须让 GPU 端不成为瓶颈，
    // 否则测出来的会是 GPU 时间而不是 CPU 时间。
    if (!LoadAndCreateMesh(device, uploadList.Get(), "assets/cube.obj",
                           m_stressMesh, stagingResources))
    {
        return false;
    }

    // SRV 槽位分配：0 = 实例贴图，1 = 屏幕文本用的字体图集
    if (!LoadAndCreateTexture(device, uploadList.Get(), "assets/checker.tga", 0,
                              m_stressTexture, stagingResources))
    {
        return false;
    }
    if (!m_debugText.Initialize(device, uploadList.Get(),
                                GetExecutableDirectory() + "assets/font_atlas.tga",
                                m_srvHeap.Get(), m_srvDescriptorSize, 1, stagingResources))
    {
        return false;
    }

    uploadList->Close();
    ID3D12CommandList* const lists[] = { uploadList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    // 等这批上传真正执行完，之后才能安全释放 staging。
    ++m_fenceValue;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue)
    {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    stagingResources.clear();

    // 材质与 GPU 资源解耦：Material 只记住「纹理在哪个 SRV 槽位」。
    m_stressMaterial.srvIndex = m_stressTexture.GetSrvIndex();

    std::cout << "[Renderer] Assets loaded: stressMesh=" << (m_stressMesh.GetIndexCount() / 3)
              << " tris, srvDescriptorSize=" << m_srvDescriptorSize << "\n";
    return true;
}

bool Renderer::LoadAndCreateMesh(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                                 const char* relativePath, Mesh& outMesh,
                                 std::vector<ComPtr<ID3D12Resource>>& stagingOut)
{
    const std::string fullPath = GetExecutableDirectory() + relativePath;

    // 第一步：把文件解析成 CPU 侧的 MeshData。
    MeshData meshData;
    std::string error;
    if (!LoadObjFromFile(fullPath, meshData, error))
    {
        std::cerr << "[Renderer] " << error << "\n";
        return false;
    }

    // 第二步：创建 GPU 缓冲并记录上传命令。
    if (!outMesh.Initialize(device, cmd, meshData, stagingOut))
    {
        std::cerr << "[Renderer] Failed to create GPU mesh from " << fullPath << "\n";
        return false;
    }
    return true;
}

bool Renderer::LoadAndCreateTexture(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                                    const char* relativePath, UINT srvIndex, Texture& outTexture,
                                    std::vector<ComPtr<ID3D12Resource>>& stagingOut)
{
    const std::string fullPath = GetExecutableDirectory() + relativePath;

    // 第一步：解码成 CPU 侧的 ImageData（统一为 RGBA8、左上原点）。
    ImageData image;
    std::string error;
    if (!LoadTgaFromFile(fullPath, image, error))
    {
        std::cerr << "[Renderer] " << error << "\n";
        return false;
    }

    // 第二步：创建显存纹理、上传、转状态，并在 SRV 堆的 srvIndex 槽位写入描述符。
    if (!outTexture.Initialize(device, cmd, image, m_srvHeap.Get(), m_srvDescriptorSize,
                               srvIndex, stagingOut))
    {
        std::cerr << "[Renderer] Failed to create GPU texture from " << fullPath << "\n";
        return false;
    }
    return true;
}

bool Renderer::CreateUiPipelineState(ID3D12Device* device)
{
    std::string errorMsg;
    std::vector<std::uint8_t> vsBytecode = ShaderCompiler::Compile(
        L"UIVS.hlsl", L"main", L"vs_6_0", errorMsg);
    if (vsBytecode.empty())
    {
        std::cerr << "[Renderer] UI vertex shader compile failed:\n" << errorMsg << "\n";
        return false;
    }
    std::vector<std::uint8_t> psBytecode = ShaderCompiler::Compile(
        L"UIPS.hlsl", L"main", L"ps_6_0", errorMsg);
    if (psBytecode.empty())
    {
        std::cerr << "[Renderer] UI pixel shader compile failed:\n" << errorMsg << "\n";
        return false;
    }

    // UI 顶点：position(float2 像素坐标) @0，uv(float2) @8，color(float4) @16
    // 对应 DebugText::UiVertex（32 字节）
    const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_rootSignature.Get();
    desc.VS = { vsBytecode.data(), vsBytecode.size() };
    desc.PS = { psBytecode.data(), psBytecode.size() };

    // Alpha 混合：字体图集是「白色字形 + alpha」，用 SrcAlpha / InvSrcAlpha 得到抗锯齿文字
    desc.BlendState.AlphaToCoverageEnable = FALSE;
    desc.BlendState.IndependentBlendEnable = FALSE;
    desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;

    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;

    // 关闭深度测试与写入：UI 永远画在最上层，不受场景深度影响
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilEnable = FALSE;

    desc.InputLayout = { inputLayout,
                         static_cast<UINT>(sizeof(inputLayout) / sizeof(inputLayout[0])) };
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    // DSV 格式必须与实际绑定的 DSV 一致，否则 PSO 创建会失败
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;

    return SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_uiPipelineState)));
}

void Renderer::RegenerateScene(std::uint32_t instanceCount)
{
    // 切换规模会立刻改变提交数量与常量缓冲内容，先确保 GPU 已经读完上一批数据。
    WaitForGpu();

    SceneConfig config = m_scene.GetConfig();
    config.instanceCount = instanceCount;

    // 立方体在局部空间的包围球半径：half = 0.7，半对角线 = 0.7 * sqrt(3)
    constexpr float kCubeLocalRadius = 0.7f * 1.73205078f;

    // meshCount / materialCount 都是 1：M7 刻意让所有实例共用同一个网格与材质，
    // 这样测出来的纯粹是「提交 N 次 draw」的 CPU 成本，不掺杂状态切换。
    m_scene.Generate(config, kCubeLocalRadius, 1, 1);

    std::cout << "[Renderer] Scene generated: " << instanceCount
              << " instances (seed=0x" << std::hex << config.randomSeed << std::dec << ")\n";
}

void Renderer::HandleKey(UINT virtualKey)
{
    constexpr float kRotateStep = 5.0f;
    constexpr float kMaxPitch = 85.0f;

    switch (virtualKey)
    {
    // ---- 场景规模 ----
    case '1': RegenerateScene(1000); break;
    case '2': RegenerateScene(10000); break;
    case '3': RegenerateScene(100000); break;

    // ---- CPU 视锥剔除开关（M8 的核心对比项）----
    case 'C':
        m_cpuCullingEnabled = !m_cpuCullingEnabled;
        std::cout << "[Renderer] CPU frustum culling: "
                  << (m_cpuCullingEnabled ? "ON  (submit visible only)"
                                          : "OFF (submit all instances)")
                  << "\n";
        break;

    // ---- 相机旋转：用来观察 Visible Count 随视角的变化 ----
    case VK_LEFT:
    case 'A':
        m_cameraYawDegrees -= kRotateStep;
        break;
    case VK_RIGHT:
    case 'D':
        m_cameraYawDegrees += kRotateStep;
        break;
    case VK_UP:
    case 'W':
        m_cameraPitchDegrees += kRotateStep;
        if (m_cameraPitchDegrees > kMaxPitch) m_cameraPitchDegrees = kMaxPitch;
        break;
    case VK_DOWN:
    case 'S':
        m_cameraPitchDegrees -= kRotateStep;
        if (m_cameraPitchDegrees < -kMaxPitch) m_cameraPitchDegrees = -kMaxPitch;
        break;

    // ---- 自动环绕（yaw 由时间推导，因此给定时刻的角度是确定的）----
    case VK_SPACE:
        m_autoOrbit = !m_autoOrbit;
        std::cout << "[Renderer] Auto orbit: " << (m_autoOrbit ? "ON" : "OFF") << "\n";
        break;

    // ---- 重置到基准视角（与 M7 的 benchmark 视角一致）----
    case 'R':
        m_cameraYawDegrees = 0.0f;
        m_cameraPitchDegrees = -11.769f; // 看向原点，对应 M7 的基准视角
        m_autoOrbit = false;
        std::cout << "[Renderer] Camera reset to baseline view\n";
        break;

    // ---- 调试可视化：关 -> 视锥 -> 视锥 + 包围球 ----
    case 'V':
        m_debugViewMode = (m_debugViewMode + 1) % 3;
        std::cout << "[Renderer] Debug view mode: " << m_debugViewMode << "\n";
        break;

    default:
        break;
    }
}

void Renderer::BuildStatisticsText()
{
    m_debugText.Begin(m_width, m_height);

    char line[192];

    // 把「生成全部统计行」封装成 lambda：先按偏移画一遍深色阴影，再画一遍亮色前景。
    // 这样无论背后是深色天空还是亮色立方体，文字都清晰可读
    // —— 这是没有 ImGui 时最简单可靠的文字可读性方案。
    const auto drawAllLines = [&](float offsetX, float offsetY)
    {
        const float x = 12.0f + offsetX;
        float y = 12.0f + offsetY;

        m_debugText.AddText(x, y, 1.0f, "CPU FRUSTUM CULLING BASELINE (M8)");
        y += 28.0f;

        std::snprintf(line, sizeof(line), "Instances : %u", m_stats.totalInstances);
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        const double visiblePercent =
            100.0 * static_cast<double>(m_stats.visibleInstances) /
            static_cast<double>(m_stats.totalInstances > 0 ? m_stats.totalInstances : 1);
        std::snprintf(line, sizeof(line), "Visible   : %u (%.1f%%)",
                      m_stats.visibleInstances, visiblePercent);
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        std::snprintf(line, sizeof(line), "Culled    : %u", m_stats.culledInstances);
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        std::snprintf(line, sizeof(line), "Draw calls: %u", m_stats.submittedDrawCalls);
        m_debugText.AddText(x, y, 1.0f, line);
        y += 26.0f;

        std::snprintf(line, sizeof(line), "CPU frame : %.2f ms", m_avgCpuFrameMs);
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        std::snprintf(line, sizeof(line), "  cull    : %.2f ms", m_avgCullMs);
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        std::snprintf(line, sizeof(line), "  update  : %.2f ms", m_avgUpdateMs);
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        std::snprintf(line, sizeof(line), "  record  : %.2f ms", m_avgRecordMs);
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        std::snprintf(line, sizeof(line), "  present : %.2f ms", m_avgPresentMs);
        m_debugText.AddText(x, y, 1.0f, line);
        y += 26.0f;

        std::snprintf(line, sizeof(line), "Culling   : %s",
                      m_cpuCullingEnabled ? "ON" : "OFF");
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        std::snprintf(line, sizeof(line), "View      : yaw %.0f  pitch %.0f%s",
                      m_cameraYawDegrees, m_cameraPitchDegrees,
                      m_autoOrbit ? "  (orbiting)" : "");
        m_debugText.AddText(x, y, 1.0f, line);
        y += 26.0f;

        static const char* const kDebugViewNames[3] = { "off", "frustum", "frustum + spheres" };
        std::snprintf(line, sizeof(line), "Debug viz : %s", kDebugViewNames[m_debugViewMode]);
        m_debugText.AddText(x, y, 1.0f, line);
        y += 26.0f;

        m_debugText.AddText(x, y, 1.0f, "[1][2][3] count   [C] culling   [V] debug viz");
        y += 18.0f;

        m_debugText.AddText(x, y, 1.0f, "[WASD/Arrows] rotate   [Space] orbit   [R] reset");
    };

    // 背景条：实例场景里模型很密集，纯文字会淹没在几何细节中，
    // 铺一层半透明黑底后统计信息在任何视角下都清晰。
    m_debugText.SetColor(0.0f, 0.0f, 0.0f, 0.55f);
    m_debugText.AddRect(4.0f, 4.0f, 508.0f, 342.0f);

    // 阴影：让白色文字在浅色模型上也有边界
    m_debugText.SetColor(0.0f, 0.0f, 0.0f, 0.85f);
    drawAllLines(1.5f, 1.5f);

    // 前景
    m_debugText.SetColor(1.0f, 1.0f, 1.0f, 1.0f);
    drawAllLines(0.0f, 0.0f);
}

bool Renderer::CreateDebugPipelineState(ID3D12Device* device)
{
    std::string errorMsg;
    std::vector<std::uint8_t> vsBytecode = ShaderCompiler::Compile(
        L"DebugVS.hlsl", L"main", L"vs_6_0", errorMsg);
    if (vsBytecode.empty())
    {
        std::cerr << "[Renderer] Debug vertex shader compile failed:\n" << errorMsg << "\n";
        return false;
    }
    std::vector<std::uint8_t> psBytecode = ShaderCompiler::Compile(
        L"DebugPS.hlsl", L"main", L"ps_6_0", errorMsg);
    if (psBytecode.empty())
    {
        std::cerr << "[Renderer] Debug pixel shader compile failed:\n" << errorMsg << "\n";
        return false;
    }

    // 调试顶点：position(float3 世界空间) @0，color(float4) @12 —— 对应 DebugLines::DebugVertex
    const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_rootSignature.Get();
    desc.VS = { vsBytecode.data(), vsBytecode.size() };
    desc.PS = { psBytecode.data(), psBytecode.size() };

    // 线框是调试信息，不做混合（保持颜色纯粹，便于分辨）
    desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;

    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // 线框没有正反面概念
    desc.RasterizerState.DepthClipEnable = TRUE;

    // 关闭深度测试与写入：调试线框要「永远可见」。
    // 被模型挡住一半的包围球没有调试价值 —— 可视化的第一诉求是看得见。
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilEnable = FALSE;

    desc.InputLayout = { inputLayout,
                         static_cast<UINT>(sizeof(inputLayout) / sizeof(inputLayout[0])) };
    // 线框用 LINELIST 拓扑；注意 D3D12 的拓扑「类型」仍然属于 TRIANGLE 家族之外，
    // 这里用 LINE 让 PSO 与后续 IASetPrimitiveTopology(LINELIST) 匹配。
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;

    return SUCCEEDED(device->CreateGraphicsPipelineState(&desc,
                                                         IID_PPV_ARGS(&m_debugPipelineState)));
}

void Renderer::UpdateCamera(double seconds)
{
    // 自动转头：yaw 完全由时间推导，因此「t 时刻的朝向」是确定的，可复现。
    if (m_autoOrbit)
    {
        constexpr float kTurnDegreesPerSecond = 20.0f;
        m_cameraYawDegrees =
            static_cast<float>(seconds) * kTurnDegreesPerSecond * m_orbitDirection;
    }

    // 相机**位置固定**在场景内部（与 M7 的基准视角一致），
    // yaw / pitch 只改变**朝向**。
    //
    // 为什么不做「绕场景中心公转」：
    //   M7/M8 的实例分布是球形各向同性的。相机绕球心公转时，
    //   视锥与球体的交集体积与朝向无关 —— 实测 Visible Count 只在
    //   7616 ~ 7709 之间波动（±0.6%），根本验证不出剔除的效果。
    //   改成「位置固定、朝向可变」后，转向场景中心与转向场景外会得到
    //   截然不同的可见数，剔除的正确性一眼可见。
    const XMFLOAT3 eye = { 0.0f, 10.0f, -48.0f };

    const float yaw = XMConvertToRadians(m_cameraYawDegrees);
    const float pitch = XMConvertToRadians(m_cameraPitchDegrees);
    const float cosPitch = std::cos(pitch);

    // 朝向：yaw = 0、pitch = 0 时指向 +Z；pitch > 0 表示抬头。
    // 默认 pitch = asin(-10 / 49.03) ≈ -11.769°，正好指向原点。
    const XMFLOAT3 target = {
        eye.x + cosPitch * std::sin(yaw),
        eye.y + std::sin(pitch),
        eye.z + cosPitch * std::cos(yaw),
    };
    const XMFLOAT3 up = { 0.0f, 1.0f, 0.0f };

    m_camera.SetView(eye, target, up);
}

void Renderer::BuildDebugVisualization(FXMMATRIX viewProj,
                                        const std::vector<std::uint32_t>& visibleIndices)
{
    m_debugLines.Begin();

    // 视锥：用 viewProj 的逆矩阵把 NDC 的 8 个角点反投影回世界空间。
    // 这是验证「平面提取是否正确」最直接的办法 —— 画出来的视锥
    // 应当恰好贴着屏幕边缘。
    const XMMATRIX inverseViewProj = XMMatrixInverse(nullptr, viewProj);
    const XMFLOAT4 frustumColor = { 1.0f, 0.85f, 0.2f, 1.0f }; // 琥珀色
    m_debugLines.AddFrustum(inverseViewProj, frustumColor);

    if (m_debugViewMode < 2)
    {
        m_debugLines.End();
        return;
    }

    // 包围球：全部画出来在 100k 实例下会淹没画面，也没有性能意义。
    // 这里各取一部分做「抽样展示」：
    //   绿色 = 通过视锥测试（可见）
    //   红色 = 被剔除
    // 抽样步长随实例总数自适应，保证画出来的球数量可控。
    const std::vector<InstanceData>& instances = m_scene.GetInstances();
    const std::size_t total = instances.size();
    if (total == 0)
    {
        m_debugLines.End();
        return;
    }

    constexpr std::size_t kMaxSpheresToDraw = 220;
    const std::size_t step = (total > kMaxSpheresToDraw) ? (total / kMaxSpheresToDraw) : 1;

    const XMFLOAT4 visibleColor = { 0.25f, 1.0f, 0.35f, 1.0f };
    const XMFLOAT4 culledColor = { 1.0f, 0.25f, 0.25f, 1.0f };

    // 可见集合是有序的，用双指针遍历避免每帧构造哈希集合
    std::size_t visibleCursor = 0;
    std::size_t drawnSpheres = 0;

    for (std::size_t i = 0; i < total && drawnSpheres < kMaxSpheresToDraw; i += step)
    {
        while (visibleCursor < visibleIndices.size() && visibleIndices[visibleCursor] < i)
        {
            ++visibleCursor;
        }
        const bool visible = (visibleCursor < visibleIndices.size() &&
                              visibleIndices[visibleCursor] == i);

        m_debugLines.AddSphere(instances[i].boundingSphere,
                               visible ? visibleColor : culledColor, 8);
        ++drawnSpheres;
    }

    m_debugLines.End();
}

