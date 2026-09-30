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
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace DirectX;

namespace
{
// CPU/GPU 路径必须使用相同的光栅化规则，才能公平比较提交方式。
// 保持基线的双面渲染；近距离或从模型内部观察时，背面也参与遮挡。
D3D12_RASTERIZER_DESC MeshRasterizerState()
{
    D3D12_RASTERIZER_DESC desc = {};
    desc.FillMode = D3D12_FILL_MODE_SOLID;
    desc.CullMode = D3D12_CULL_MODE_NONE;
    desc.FrontCounterClockwise = FALSE;
    desc.DepthClipEnable = TRUE;
    return desc;
}

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
                          bool useCpuCulling, float cameraYawDegrees, int debugViewMode,
                          bool compareCullingAtStartup, bool gpuDriven)
{
    m_cpuCullingEnabled = useCpuCulling;
    m_cameraYawDegrees = cameraYawDegrees;
    m_debugViewMode = debugViewMode;
    m_renderMode = gpuDriven ? RenderMode::GpuDriven : RenderMode::CpuDriven;
    m_device = device; // 场景切换时需要重新上传实例数据

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

    if (!CreateGpuDrivenRootSignature(device)) return false;

    if (!CreateGpuDrivenPipelineState(device)) return false;

    if (!CreateDepthBuffer(device)) return false;
    if (!CreateConstantBuffers(device)) return false;


    // 相机：位置固定在场景内部（场景半径 55）。
    // 身后与视锥外的大量实例会被真正剔除，Visible Count 才有意义；
    // 若把相机远远放在球外，整个场景都落在视锥里，可见率恒为 100%。
    m_camera.SetPerspective(60.0f, static_cast<float>(m_width) / static_cast<float>(m_height),
                            0.5f, 500.0f);
    UpdateCamera(0.0);

    // 初始场景规模来自命令行（默认 1000），运行中按 1 / 2 / 3 可切换。
    // **顺序很重要**：必须先完成 CPU 侧场景生成，CreateAssets 才能把
    // 实例数据一并上传到 GPU（M9）。
    RegenerateScene(initialInstanceCount);

    if (!CreateAssets(device)) return false;
    if (!m_debugLines.Initialize(device)) return false;

    // M9：上传完成后立刻验证一次，确认 C++ / HLSL 的布局约定一致。
    RunInstanceValidation();

    // M10：--compare-cull 时，把「GPU vs CPU 剔除」的对比排到稍后的一帧。
    // 必须先有一帧算出 CPU 结果与视锥平面，对比才有参照；
    // 而 GPU 侧的压缩结果同样来自**已经执行完**的帧 —— 所以这里再等几帧，
    // 避免在「GPU 还没来得及写」时就读回（那会读到全 0）。
    m_cullComparisonRequested = false;
    m_compareCullingAtStartup = compareCullingAtStartup;

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

    // M10：按 G 键请求的 GPU/CPU 剔除结果对比，在 WaitForGpu 之后执行。
    // 注意真正的对比放在**本帧 CPU 剔除完成之后**（见下面），
    // 这样两侧用的才是同一帧、同一份视锥平面。
    if (m_validationRequested)
    {
        m_validationRequested = false;
        RunInstanceValidation();
    }

    // --compare-cull 延迟到第 60 帧：必须等 GPU 真正执行过命令生成与压缩，
    // 读回才有意义（第一帧读回只会得到缓冲区初值 0）。
    if (m_compareCullingAtStartup && m_frameCounter >= 60u)
    {
        m_compareCullingAtStartup = false;
        m_cullComparisonRequested = true;
    }
    ++m_frameCounter;

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
    //
    //    M10 起，平面被单独提取出来保存：GPU 剔除（FrustumCullingCS）会用
    //    **完全相同的一份平面**，这样「GPU vs CPU 的可见集是否一致」
    //    才是一个纯粹的浮点运算对比，而不掺杂平面提取方式的差异。
    const std::vector<InstanceData>& instances = m_scene.GetInstances();
    LARGE_INTEGER counterCullBegin = {};
    QueryPerformanceCounter(&counterCullBegin);
    ExtractFrustumPlanes(viewProj, m_lastFrustumPlanes);
    CullInstancesByFrustumWithPlanes(instances, m_lastFrustumPlanes, m_visibleIndices);
    QueryPerformanceCounter(&counterCullEnd);

    const std::uint32_t totalCount = static_cast<std::uint32_t>(instances.size());
    const std::uint32_t visibleCount = static_cast<std::uint32_t>(m_visibleIndices.size());

    // 3b. M10：按 G 键时，用**刚算出的这一份平面**重跑一次 GPU 剔除并读回对比。
    //
    //     为什么要在对比里重新 dispatch，而不是复用主命令列表里那份：
    //     主命令列表的 dispatch 要到本帧稍后才执行，而读回需要一个
    //     「dispatch -> UAV barrier -> copy -> 等待」的完整闭环。
    //     在这里用独立命令列表重跑一次，可以保证两侧输入完全相同
    //     （同一帧、同一份归一化平面），对比结论才严格成立。
    if (m_cullComparisonRequested && totalCount > 0)
    {
        m_cullComparisonRequested = false;
        CompareGpuAndCpuCulling();
    }

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

    // M12：GPU-Driven 路径的**全局**常量（每帧一份）。
    // 它替代了「每实例一份 ObjectConstants」—— 实例的 world 矩阵改从
    // 常驻显存的 StructuredBuffer 读取，只有全场共用的 viewProj 与
    // 光照方向还需要每帧上传一次。
    {
        GlobalConstants globalConstants = {};
        XMStoreFloat4x4(&globalConstants.viewProj, viewProj);
        globalConstants.lightDirection = lightDirection;
        std::memcpy(constantBase + static_cast<std::size_t>(kGlobalConstantSlot) * kConstantStride,
                    &globalConstants, sizeof(globalConstants));
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
    //
    //     M10 在图形管线之前插入了一个 Compute Pass（GPU 视锥剔除）。
    //     顺序上必须注意：compute 与 graphics 各自有自己的根签名/PSO 绑定，
    //     所以下面重新 SetGraphicsRootSignature + SetPipelineState 是必要的 ——
    //     不能假设图形状态还留在上一帧的设置上。
    ID3D12DescriptorHeap* const descriptorHeaps[] = { m_srvHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, descriptorHeaps);

    // 10a. GPU 视锥剔除 + Stream Compaction：每线程一个实例，
    //      结果写进「压缩后的可见索引列表 + 计数器」。
    //      Dispatch 的线程组数 = ceil(instanceCount / 64)，见 GPUFrustumCuller。
    if (totalCount > 0)
    {
        m_frustumCuller.Record(m_commandList.Get(), m_srvHeap.Get(), m_srvDescriptorSize,
                               kInstanceSrvSlot, m_visibleList,
                               m_lastFrustumPlanes, totalCount);
    }

    // 10b. GPU 命令生成：为每个可见实例生成一条 DrawIndexed 间接命令。
    //
    //      它读 10a 产出的压缩列表与计数器，写 indirect argument buffer。
    //      **线程组数按容量上限取整**，实际写多少条由 GPU 内部的可见数决定 ——
    //      这是 GPU-Driven 的核心特征：CPU 不知道也不需要知道命令条数。
    if (totalCount > 0)
    {
        m_indirectCommands.Record(m_commandList.Get(), m_srvHeap.Get(), m_srvDescriptorSize,
                                  m_visibleList,
                                  m_stressMesh.GetIndexCount());
    }

    // 10c. 切到图形管线。两条路径各自的绑定完全不同，所以这里是分支点：
    //
    //      CPU-Driven：主根签名 + 网格 PSO，逐个实例 SetCBV + Draw（第 11 步的循环）
    //      GPU-Driven：独立的根签名 + PSO，然后**一次** ExecuteIndirect
    const D3D12_VERTEX_BUFFER_VIEW& vertexBufferView = m_stressMesh.GetVertexBufferView();
    const D3D12_INDEX_BUFFER_VIEW& indexBufferView = m_stressMesh.GetIndexBufferView();
    const UINT indexCount = m_stressMesh.GetIndexCount();

    if (m_renderMode == RenderMode::GpuDriven)
    {
        m_stats.gpuDriven = true;
        m_stats.submittedDrawCalls = 0;    // CPU 侧一次 draw 都没有提交
        m_stats.indirectExecuteCount = 1;  // 只有一次 ExecuteIndirect

        RecordGpuDrivenDraw(indexCount, submitCount);
    }
    else
    {
        m_stats.gpuDriven = false;
        m_stats.submittedDrawCalls = submitCount;
        m_stats.indirectExecuteCount = 0;

        m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        m_commandList->SetPipelineState(m_pipelineState.Get());

        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        m_commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
        m_commandList->IASetIndexBuffer(&indexBufferView);

        D3D12_GPU_DESCRIPTOR_HANDLE materialSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        materialSrv.ptr += static_cast<UINT64>(m_stressMaterial.srvIndex) * m_srvDescriptorSize;
        m_commandList->SetGraphicsRootDescriptorTable(1, materialSrv);

        const D3D12_GPU_VIRTUAL_ADDRESS constantBaseAddress =
            frame.constantBuffer.GetGPUVirtualAddress();

        // **baseline 的核心**：每个实例一次 SetGraphicsRootConstantBufferView + 一次 Draw。
        for (std::uint32_t i = 0; i < submitCount; ++i)
        {
            m_commandList->SetGraphicsRootConstantBufferView(
                0, constantBaseAddress + static_cast<UINT64>(i) * kConstantStride);
            m_commandList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
        }
    }

    // 12. 调试线框（视锥 / 包围球）：换 PSO（LINELIST 拓扑、关闭深度测试），其余状态复用。
    //
    //     注意它用的是**主根签名**，所以 GPU-Driven 模式下必须先切回来 ——
    //     否则管线状态与当前根签名不匹配（Debug Layer 会直接报错）。
    if (m_debugLines.HasContent())
    {
        m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        m_commandList->SetPipelineState(m_debugPipelineState.Get());
        m_commandList->SetGraphicsRootConstantBufferView(
            0, frame.constantBuffer.GetGPUVirtualAddress() +
                   static_cast<UINT64>(kDebugConstantSlot) * kConstantStride);
        m_debugLines.Render(m_commandList.Get());

        // Render() 把拓扑改成了 LINELIST，后面 UI 用的是三角形，必须改回来
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    // 13. UI 叠加：切换 PSO（alpha 混合、关闭深度），其余状态复用。
    if (m_debugText.HasContent())
    {
        m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        m_commandList->SetPipelineState(m_uiPipelineState.Get());

        D3D12_GPU_DESCRIPTOR_HANDLE fontSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        fontSrv.ptr += static_cast<UINT64>(m_debugText.GetSrvIndex()) * m_srvDescriptorSize;
        m_commandList->SetGraphicsRootDescriptorTable(1, fontSrv);

        m_commandList->SetGraphicsRootConstantBufferView(
            0, frame.constantBuffer.GetGPUVirtualAddress() +
                   static_cast<UINT64>(kUiConstantSlot) * kConstantStride);
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
    // 注意 submittedDrawCalls 与 indirectExecuteCount 在**记录命令时**
    // 已经按当前渲染模式设好了（见第 10c 步的分支），这里不能覆盖 ——
    // 否则 GPU-Driven 模式下会把「CPU 实际一次都没提交」误报成 N 次。
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
                  << " mode=" << (m_stats.gpuDriven ? "gpu" : "cpu")
                  << " cpuDraws=" << m_stats.submittedDrawCalls
                  << " execIndirect=" << m_stats.indirectExecuteCount
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
    desc.RasterizerState = MeshRasterizerState();
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
    // 槽位：0..kMaxInstances-1 = 实例，kUiConstantSlot = UI，
    //       kDebugConstantSlot = 调试线框，kGlobalConstantSlot = GPU-Driven 全局常量（M12）
    //
    // 注意这里用的是「最后一个槽位的下标 + 1」——
    // 每加一个新的专用槽位都必须同步这个表达式，否则会出现
    // 「槽位下标有效但缓冲装不下」的越界（表现是绑定 CBV 时直接崩溃）。
    constexpr UINT kLastConstantSlot = kGlobalConstantSlot;
    const std::uint64_t bufferSize =
        static_cast<std::uint64_t>(kLastConstantSlot + 1) * kConstantStride;

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

    // 描述符槽位分配见 Renderer.h 的 kXxxSrvSlot 常量
    if (!LoadAndCreateTexture(device, uploadList.Get(), "assets/checker.tga",
                              kStressTextureSrvSlot, m_stressTexture, stagingResources))
    {
        return false;
    }
    if (!m_debugText.Initialize(device, uploadList.Get(),
                                GetExecutableDirectory() + "assets/font_atlas.tga",
                                m_srvHeap.Get(), m_srvDescriptorSize,
                                kFontAtlasSrvSlot, stagingResources))
    {
        return false;
    }

    // ---- M9：把整个场景的实例数据放进 DEFAULT Heap 的 StructuredBuffer ----
    //
    // 注意必须在 uploadList->Close() 之前记录命令：这里的上传走的是同一批
    // 「初始化期一次性上传 -> 提交 -> 等待」的流程。
    // 场景在 Initialize 中已经先生成好了（见那里的顺序说明）。
    if (!m_instanceBuffer.Initialize(device, uploadList.Get(), kMaxInstances,
                                     m_scene.GetInstances(), m_srvHeap.Get(),
                                     m_srvDescriptorSize, kInstanceSrvSlot, stagingResources))
    {
        std::cerr << "[Renderer] Failed to create GPU instance buffer.\n";
        return false;
    }

    // 验证器（M9 的 Debug Validation）：创建 UAV 缓冲与 Compute PSO，
    // 本身不上传数据，所以放在这里不影响上面的 staging 生命周期。
    if (!m_instanceValidator.Initialize(device, kMaxInstances, m_srvHeap.Get(),
                                        m_srvDescriptorSize,
                                        kValidationResultsUavSlot, kValidationDumpUavSlot))
    {
        std::cerr << "[Renderer] Failed to create instance validator.\n";
        return false;
    }

    // ---- M10/M11：GPU 视锥剔除 + Stream Compaction 所需的资源与 Compute PSO ----
    // VisibleInstanceList：压缩后的可见实例 ID 列表 + 计数器（都是 DEFAULT Heap + UAV）
    if (!m_visibleList.Initialize(device, kMaxInstances, m_srvHeap.Get(),
                                  m_srvDescriptorSize, kVisibilityUavSlot, kVisibleCountUavSlot,
                                  kVisibleIndicesSrvSlot, kVisibleCountSrvSlot))
    {
        std::cerr << "[Renderer] Failed to create visible instance list.\n";
        return false;
    }
    if (!m_frustumCuller.Initialize(device, kMaxInstances))
    {
        std::cerr << "[Renderer] Failed to create GPU frustum culler.\n";
        return false;
    }

    // ---- M12：GPU-Driven 渲染所需的间接命令基础设施 ----
    if (!m_indirectCommands.Initialize(device, kMaxInstances, m_srvHeap.Get(),
                                       m_srvDescriptorSize, kIndirectArgsUavSlot,
                                       kVisibleIndicesSrvSlot, kVisibleCountSrvSlot))
    {
        std::cerr << "[Renderer] Failed to create indirect draw commands.\n";
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
    std::cout << "[Renderer] GPU instance buffer: " << m_instanceBuffer.GetInstanceCount()
              << " instances x " << InstanceBuffer::GetStride() << " bytes = "
              << (static_cast<double>(m_instanceBuffer.GetInstanceCount()) *
                  InstanceBuffer::GetStride() / 1024.0 / 1024.0)
              << " MB (DEFAULT heap, capacity " << m_instanceBuffer.GetCapacity() << ")\n";
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

// ---------------------------------------------------------------------------
// M12：GPU-Driven 渲染路径的根签名与 PSO
//
// 这套绑定刻意与 CPU-Driven 的根签名**分开**，原因有两个：
//   ① GPU-Driven 没有「每实例常量」这个概念，b0 的语义完全不同
//      （全局相机常量 vs 每实例变换），塞进同一个根签名会让两边都难读；
//   ② 分离之后 CPU-Driven 那条已经验证过的路径完全不受影响，
//      两种模式可以并存并随时切换比较。
//
// 根签名布局：
//   参数 0：CBV @ b0        全局常量（viewProj + lightDirection）   ALL
//   参数 1：SRV 表 @ t0     实例数据 StructuredBuffer<InstanceData> ALL
//   参数 2：SRV 表 @ t1     压缩后的可见实例索引（M11 产出）        ALL
//   参数 3：SRV 表 @ t2     材质纹理                                PIXEL
//   静态采样器 s0                                                   PIXEL
//
//   注意 t0/t1/t2 是 **shader register**，与描述符堆里的槽位无关：
//   这三张表各自指向堆槽位 2、3、0 —— 相互独立，因此**不需要**在堆里相邻。
// ---------------------------------------------------------------------------
bool Renderer::CreateGpuDrivenRootSignature(ID3D12Device* device)
{
    D3D12_ROOT_PARAMETER params[4] = {};

    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0; // b0
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE instanceRange = {};
    instanceRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    instanceRange.NumDescriptors = 1;
    instanceRange.BaseShaderRegister = 0; // t0
    instanceRange.RegisterSpace = 0;
    instanceRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &instanceRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE visibleRange = {};
    visibleRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    visibleRange.NumDescriptors = 1;
    visibleRange.BaseShaderRegister = 1; // t1
    visibleRange.RegisterSpace = 0;
    visibleRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &visibleRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE materialRange = {};
    materialRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    materialRange.NumDescriptors = 1;
    materialRange.BaseShaderRegister = 2; // t2
    materialRange.RegisterSpace = 0;
    materialRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &materialRange;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = 4;
    rootDesc.pParameters = params;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &signature, &error)))
    {
        std::cerr << "[Renderer] GPU-driven root signature serialize failed.\n";
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(),
                                           signature->GetBufferSize(),
                                           IID_PPV_ARGS(&m_gpuDrivenRootSignature))))
    {
        std::cerr << "[Renderer] GPU-driven CreateRootSignature failed.\n";
        return false;
    }
    return true;
}

bool Renderer::CreateGpuDrivenPipelineState(ID3D12Device* device)
{
    std::string errorMsg;
    std::vector<std::uint8_t> vsBytecode = ShaderCompiler::Compile(
        L"MeshGPUDrivenVS.hlsl", L"main", L"vs_6_0", errorMsg);
    if (vsBytecode.empty())
    {
        std::cerr << "[Renderer] GPU-driven VS compile failed:\n" << errorMsg << "\n";
        return false;
    }
    std::vector<std::uint8_t> psBytecode = ShaderCompiler::Compile(
        L"MeshGPUDrivenPS.hlsl", L"main", L"ps_6_0", errorMsg);
    if (psBytecode.empty())
    {
        std::cerr << "[Renderer] GPU-driven PS compile failed:\n" << errorMsg << "\n";
        return false;
    }

    // 输入布局与 CPU-Driven 的网格 PSO 完全一致（同一个 Mesh）
    const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_gpuDrivenRootSignature.Get();
    desc.VS = { vsBytecode.data(), vsBytecode.size() };
    desc.PS = { psBytecode.data(), psBytecode.size() };

    desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;

    desc.RasterizerState = MeshRasterizerState();

    // 深度状态必须与 CPU-Driven 的网格 PSO 一致，否则切换模式时画面会变
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DepthStencilState.StencilEnable = FALSE;

    desc.InputLayout = { inputLayout,
                         static_cast<UINT>(sizeof(inputLayout) / sizeof(inputLayout[0])) };
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;

    return SUCCEEDED(device->CreateGraphicsPipelineState(&desc,
                                                         IID_PPV_ARGS(&m_gpuDrivenPipelineState)));
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

    // M9：把新的实例数据重新上传到 GPU。
    // 这里只在场景规模变化时发生（初始化或按 1/2/3），不在每帧路径上。
    if (m_instanceBuffer.GetCapacity() > 0)
    {
        UploadInstanceData();
    }
}

bool Renderer::UploadInstanceData()
{
    const std::vector<InstanceData>& instances = m_scene.GetInstances();
    if (instances.empty())
    {
        return true;
    }

    // 复用初始化期那套「临时命令列表 -> 提交 -> 等待」流程：
    // 一次性、不在热路径上，代码直白比省一点延迟更重要。
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&allocator))) ||
        FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        std::cerr << "[Renderer] Failed to create upload list for instance data.\n";
        return false;
    }

    std::vector<ComPtr<ID3D12Resource>> stagingResources;
    if (!m_instanceBuffer.Upload(m_device.Get(), list.Get(), instances, stagingResources))
    {
        std::cerr << "[Renderer] Failed to upload instance data.\n";
        return false;
    }

    list->Close();
    ID3D12CommandList* const lists[] = { list.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    ++m_fenceValue;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue)
    {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    stagingResources.clear();

    std::cout << "[Renderer] Instance data uploaded to GPU: " << instances.size()
              << " x " << InstanceBuffer::GetStride() << " B ("
              << (static_cast<double>(instances.size()) * InstanceBuffer::GetStride() / 1024.0 / 1024.0)
              << " MB)\n";
    return true;
}

void Renderer::RunInstanceValidation()
{
    const std::uint32_t instanceCount = m_instanceBuffer.GetInstanceCount();
    if (instanceCount == 0)
    {
        return;
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&allocator))) ||
        FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        std::cerr << "[Validation] Failed to create command list.\n";
        return;
    }

    // 验证 CS 要读 StructuredBuffer、写两个 UAV，因此这几张表必须在
    // shader-visible 堆里，且必须先 SetDescriptorHeaps 才能解释堆内句柄。
    ID3D12DescriptorHeap* const heaps[] = { m_srvHeap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    m_instanceValidator.Record(list.Get(), m_srvHeap.Get(), m_srvDescriptorSize,
                               kInstanceSrvSlot, kValidationResultsUavSlot, instanceCount);

    list->Close();
    ID3D12CommandList* const lists[] = { list.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    // 必须等 GPU 真正写完 UAV 并完成 copy，才能 Map readback 缓冲。
    ++m_fenceValue;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue)
    {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    m_instanceValidator.Report(m_scene.GetInstances());
}

void Renderer::CompareGpuAndCpuCulling()
{
    const std::uint32_t instanceCount = m_instanceBuffer.GetInstanceCount();
    if (instanceCount == 0)
    {
        return;
    }

    // ---- 用独立命令列表跑一次 GPU 剔除，然后读回 ----
    //
    // 这一次 dispatch 与主渲染命令列表里那份是重复的，但只有按下验证键时
    // 才会发生，换来的是「GPU 与 CPU 使用同一帧、同一份平面」这个严格前提。
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&allocator))) ||
        FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        std::cerr << "[Renderer] Failed to create command list for culling comparison.\n";
        return;
    }

    ID3D12DescriptorHeap* const heaps[] = { m_srvHeap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    m_frustumCuller.Record(list.Get(), m_srvHeap.Get(), m_srvDescriptorSize,
                           kInstanceSrvSlot, m_visibleList,
                           m_lastFrustumPlanes, instanceCount);
    list->Close();

    ID3D12CommandList* const lists[] = { list.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    ++m_fenceValue;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue)
    {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    // ---- 读回并与本帧的 CPU 可见集逐实例对比 ----
    m_frustumCuller.ReadbackAndCompare(m_device.Get(), m_commandQueue.Get(),
                                       m_fence.Get(), m_fenceEvent, m_fenceValue,
                                       m_visibleList,
                                       m_visibleIndices, instanceCount);

    // 临时诊断：此刻 GPU 已经执行过若干帧，参数缓冲里应当有真实内容。
    m_indirectCommands.DebugReadbackFirstCommands(
        m_device.Get(), m_commandQueue.Get(), m_fence.Get(), m_fenceEvent,
        m_fenceValue, 4);
}

void Renderer::RecordGpuDrivenDraw(UINT indexCount, std::uint32_t visibleCount)
{
    // visibleCount 在本版里不再参与提交 —— ExecuteIndirect 的命令条数恒为 1，
    // 而「画多少个实例」由 GPU 写进间接命令的 InstanceCount 决定。
    // 参数保留是为了给调用方一个明确的语义：CPU **知道**可见数，但**不用**它。
    (void)visibleCount;

    // =========================================================================
    // GPU-Driven 的绘制记录：整段只有**一次** ExecuteIndirect，没有任何逐实例循环。
    // =========================================================================
    // 1. 绑定 GPU-Driven 专用的根签名与 PSO。
    m_commandList->SetGraphicsRootSignature(m_gpuDrivenRootSignature.Get());
    m_commandList->SetPipelineState(m_gpuDrivenPipelineState.Get());

    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 2. 几何数据（顶点/索引缓冲）—— 所有实例共用同一个 Mesh（本版的简化）。
    const D3D12_VERTEX_BUFFER_VIEW& vertexBufferView = m_stressMesh.GetVertexBufferView();
    const D3D12_INDEX_BUFFER_VIEW& indexBufferView = m_stressMesh.GetIndexBufferView();
    m_commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
    m_commandList->IASetIndexBuffer(&indexBufferView);

    // 3. 绑定：全局常量(b0) + 实例数据(t0) + 可见索引(t1) + 材质(t2)
    //
    //    与 CPU-Driven 最大的不同：**没有每实例的常量绑定**。
    //    实例的 world 矩阵由顶点着色器用 SV_InstanceID 从 StructuredBuffer 自取。
    m_commandList->SetGraphicsRootConstantBufferView(
        0, m_frames[m_frameIndex].constantBuffer.GetGPUVirtualAddress() +
               static_cast<UINT64>(kGlobalConstantSlot) * kConstantStride);

    D3D12_GPU_DESCRIPTOR_HANDLE instanceSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    instanceSrv.ptr += static_cast<UINT64>(kInstanceSrvSlot) * m_srvDescriptorSize;
    m_commandList->SetGraphicsRootDescriptorTable(1, instanceSrv);

    // 压缩后的可见实例索引（M11 产出）：顶点着色器用它把 SV_InstanceID
    // 换算成真正的实例 ID。
    D3D12_GPU_DESCRIPTOR_HANDLE visibleSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    visibleSrv.ptr += static_cast<UINT64>(kVisibleIndicesSrvSlot) * m_srvDescriptorSize;
    m_commandList->SetGraphicsRootDescriptorTable(2, visibleSrv);

    D3D12_GPU_DESCRIPTOR_HANDLE materialSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    materialSrv.ptr += static_cast<UINT64>(m_stressMaterial.srvIndex) * m_srvDescriptorSize;
    m_commandList->SetGraphicsRootDescriptorTable(3, materialSrv);

    // 4. **状态转换**：参数缓冲从 UNORDERED_ACCESS 转到 INDIRECT_ARGUMENT。
    //
    //    ExecuteIndirect 要求参数缓冲处于 D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT ——
    //    它告诉驱动这块内存接下来由**命令处理器**读取，而不是被着色器读写。
    //    用错状态会读到陈旧的命令（驱动会认为不需要刷新相关缓存）。
    m_indirectCommands.TransitionTo(m_commandList.Get(),
                                    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    // 5. 可见索引列表在这次绘制里被**顶点着色器**读取，所以要转到
    //    NON_PIXEL_SHADER_RESOURCE（顶点阶段属于 non-pixel）。
    m_visibleList.TransitionIndicesTo(m_commandList.Get(),
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_visibleList.TransitionCountTo(m_commandList.Get(),
                                    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    // 6. **一次 ExecuteIndirect 提交 1 条命令**，而这条命令的 InstanceCount
    //    是 GPU 写进去的可见数 —— 于是「画多少个实例」完全由 GPU 决定。
    //
    //    MaxCommandCount = 1 是编译期常数：无论场景有多少实例、可见多少，
    //    CPU 提交的命令条数**永远是 1**。这就是 GPU-Driven 的实质。
    m_commandList->ExecuteIndirect(
        m_indirectCommands.GetCommandSignature(),
        1,                                         // 永远只有一条命令
        m_indirectCommands.GetArgumentBuffer(), 0,
        nullptr, 0);                               // 条数不来自 count buffer

    // 7. 转回 UNORDERED_ACCESS，让下一帧的命令生成 CS 可以直接继续写。
    m_indirectCommands.TransitionTo(m_commandList.Get(),
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);


    (void)indexCount;
}

void Renderer::HandleKey(UINT virtualKey){
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

    // ---- M9：在 GPU 上重新验证实例数据 ----
    // 只置位，真正的执行放在下一帧的安全点（WaitForGpu 之后），
    // 避免与主渲染的命令列表/allocator 冲突。
    case 'T':
        m_validationRequested = true;
        break;

    // ---- M10：对比 GPU 与 CPU 的视锥剔除结果 ----
    // 同样只置位，在下一帧 CPU 剔除完成后执行（那时两侧输入才是同帧同源的）。
    case 'G':
        m_cullComparisonRequested = true;
        break;

    // ---- M12：切换渲染路径 ----
    // CPU-Driven：CPU 对每个可见实例发一次 SetCBV + DrawIndexedInstanced
    // GPU-Driven：CPU 只发一次 ExecuteIndirect，命令条数由 GPU 计数决定
    case 'M':
        m_renderMode = (m_renderMode == RenderMode::CpuDriven)
                           ? RenderMode::GpuDriven
                           : RenderMode::CpuDriven;
        std::cout << "[Renderer] Render mode: "
                  << (m_renderMode == RenderMode::GpuDriven
                          ? "GPU-DRIVEN (ExecuteIndirect)"
                          : "CPU-DRIVEN (per-instance draw)")
                  << "\n";
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
        y += 18.0f;

        // M12：渲染路径与 GPU 侧的 draw 提交方式 —— 本里程碑最核心的一组对比数字。
        //   CPU-Driven：CPU 逐个实例提交，draws = 可见实例数
        //   GPU-Driven：CPU 一次 draw 都没提交，只有 1 次 ExecuteIndirect；
        //               真正的实例数由 GPU 写进间接命令里
        if (m_stats.gpuDriven)
        {
            std::snprintf(line, sizeof(line), "Mode      : GPU-DRIVEN (1 indirect cmd)");
        }
        else
        {
            std::snprintf(line, sizeof(line), "Mode      : CPU-DRIVEN (%u draws)",
                          m_stats.submittedDrawCalls);
        }
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        std::snprintf(line, sizeof(line), "CPU draws : %u   ExecIndirect: %u",
                      m_stats.submittedDrawCalls, m_stats.indirectExecuteCount);
        m_debugText.AddText(x, y, 1.0f, line);
        y += 26.0f;

        // M9：确认实例数据已经在 GPU 上（DEFAULT Heap 的 StructuredBuffer）
        std::snprintf(line, sizeof(line), "GPU inst  : %u x %u B",
                      m_instanceBuffer.GetInstanceCount(), InstanceBuffer::GetStride());
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        // M10/M11：GPU 视锥剔除 + Stream Compaction 的规模。
        // 线程组数 = ceil(instanceCount / 64)，加号后面的 63 就是「向上取整」。
        std::snprintf(line, sizeof(line), "GPU cull  : %u threads / %u groups",
                      m_instanceBuffer.GetInstanceCount(),
                      GPUFrustumCuller::GetGroupCount(m_instanceBuffer.GetInstanceCount()));
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        // M11：压缩列表的容量。实际有效长度由 GPU 计数器给出，
        // 但那个值在 GPU 上，读回会造成 CPU 同步 —— 正常渲染流程里不读它。
        std::snprintf(line, sizeof(line), "Compact   : %u idx cap / %u B counter",
                      m_visibleList.GetCapacity(), VisibleInstanceList::GetCountStride());
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        // M11：最近一次 GPU/CPU 压缩对比结果（按 G 键才更新，因为需要回读）
        if (m_frustumCuller.HasComparisonResult())
        {
            std::snprintf(line, sizeof(line), "CPU/GPU vis: %u / %u  mismatch %u",
                          m_frustumCuller.GetLastCpuVisibleCount(),
                          m_frustumCuller.GetLastGpuVisibleCount(),
                          m_frustumCuller.GetLastMismatchCount());
        }
        else
        {
            std::snprintf(line, sizeof(line), "CPU/GPU vis: (press G to compare)");
        }
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

        m_debugText.AddText(x, y, 1.0f, "[1][2][3] count  [C] culling  [V] viz  [T] validate");
        y += 18.0f;

        m_debugText.AddText(x, y, 1.0f, "[G] compare cull  [M] CPU/GPU mode  [V] viz  [T] validate");
        y += 18.0f;

        m_debugText.AddText(x, y, 1.0f, "[1][2][3] count   [C] culling   [Space] orbit   [R] reset");
    };

    // 背景条：实例场景里模型很密集，纯文字会淹没在几何细节中，
    // 铺一层半透明黑底后统计信息在任何视角下都清晰。
    m_debugText.SetColor(0.0f, 0.0f, 0.0f, 0.55f);
    m_debugText.AddRect(4.0f, 4.0f, 620.0f, 500.0f);

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

