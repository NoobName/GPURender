#include "Render/Renderer.h"

#include "Asset/ObjLoader.h"
#include "Asset/TgaLoader.h"
#include "Render/ShaderCompiler.h"
#include "Render/UploadHelper.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
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
                          bool compareCullingAtStartup, bool gpuDriven,
                          bool depthPrepass, bool depthVisualize,
                          bool hzbVisualize, std::uint32_t hzbMip,
                          bool occlusion, bool occlusionViz)
{
    m_cpuCullingEnabled = useCpuCulling;
    m_cameraYawDegrees = cameraYawDegrees;
    m_debugViewMode = debugViewMode;
    m_renderMode = gpuDriven ? RenderMode::GpuDriven : RenderMode::CpuDriven;
    m_depthPrepassEnabled = depthPrepass;
    m_depthVisualizeEnabled = depthVisualize;
    m_hzbVisualizeEnabled = hzbVisualize;
    m_hzbViewMip = hzbMip;
    m_occlusionEnabled = occlusion;
    m_occlusionVisualize = occlusionViz;
    // 遮挡剔除可视化本身依赖「包围球」这一层的调试线框，
    // 所以开启它时自动把 debug viz 提到第 2 级（视锥 + 包围球）。
    if (occlusionViz && m_debugViewMode < 2)
    {
        m_debugViewMode = 2;
    }
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
    if (!CreateSrvDescriptorHeap(device)) return false;
    if (!CreateRootSignature(device)) return false;
    if (!CreatePipelineState(device)) return false;
    if (!CreateUiPipelineState(device)) return false;
    if (!CreateDebugPipelineState(device)) return false;
    if (!CreateGpuDrivenRootSignature(device)) return false;
    if (!CreateGpuDrivenPipelineState(device)) return false;
    if (!CreateDepthBuffer(device)) return false;
    if (!CreateConstantBuffers(device)) return false;
    if (!CreateDepthOnlyPipelineState(device)) return false;      // M13
    if (!CreateDepthVisualizePipelineState(device)) return false; // M13
    if (!CreateHZBVisualizePipelineState(device)) return false;   // M14

    // M14：层级深度金字塔。
    // 必须放在 CreateDepthBuffer 之后 —— 它的第 0 级是从深度缓冲降采样得到的。
    if (!m_hzb.Initialize(device, m_width, m_height, m_srvHeap.Get(),
                          m_srvDescriptorSize, kHZBFirstMipUavSlot, kHZBSrvSlot))
    {
        std::cerr << "[Renderer] Failed to initialize HZB.\n";
        return false;
    }
    m_hzbViewMip = 0;

    // M15：HZB 遮挡剔除
    if (!m_occlusionCuller.Initialize(device, kMaxInstances, m_srvHeap.Get(),
                                     m_srvDescriptorSize,
                                     kOcclusionStatsUavSlot, kOcclusionStatsSrvSlot))
    {
        std::cerr << "[Renderer] Failed to initialize occlusion culler.\n";
        return false;
    }

    if (!m_gpuProfiler.Initialize(device, m_commandQueue.Get())) return false;


    // 相机：位置固定在场景内部（场景半径 55）。
    // 身后与视锥外的大量实例会被真正剔除，Visible Count 才有意义；
    // 若把相机远远放在球外，整个场景都落在视锥里，可见率恒为 100%。
    m_camera.SetPerspective(60.0f, static_cast<float>(m_width) / static_cast<float>(m_height),
                            m_nearPlane, m_farPlane);
    UpdateCamera(0.0);

    // 初始场景规模来自命令行（默认 1000），运行中按 1 / 2 / 3 可切换。
    // **顺序很重要**：必须先完成 CPU 侧场景生成，CreateAssets 才能把
    // 实例数据一并上传到 GPU（M9）。
    RegenerateScene(initialInstanceCount);

    if (!CreateAssets(device)) return false;
    if (!m_debugLines.Initialize(device)) return false;
    // M14：一次性验证用的 readback 缓冲（不参与 HZB 的生成）
    if (!CreateHZBVerifyReadback(device)) return false;
    if (!CreateOcclusionDebugReadback(device)) return false; // M15

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

    // M13：Depth Prepass 开关切换后需要重建 PSO
    //（Main Pass 的深度状态是烘焙在 PSO 里的）。
    if (m_pipelineStateRebuildRequested)
    {
        m_pipelineStateRebuildRequested = false;
        if (!CreatePipelineState(m_device.Get()) ||
            !CreateGpuDrivenPipelineState(m_device.Get()))
        {
            std::cerr << "[Renderer] Failed to rebuild PSOs after prepass toggle.\n";
        }
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

    // M13：深度可视化的常量（near/far + 1/分辨率）
    {
        struct DepthVisualizeConstants
        {
            XMFLOAT2 invResolution;
            float nearPlane;
            float farPlane;
        };
        DepthVisualizeConstants depthConstants = {};
        depthConstants.invResolution = { 1.0f / static_cast<float>(m_width),
                                         1.0f / static_cast<float>(m_height) };
        depthConstants.nearPlane = m_nearPlane;
        depthConstants.farPlane = m_farPlane;
        std::memcpy(constantBase +
                        static_cast<std::size_t>(kDepthVisualizeConstantSlot) * kConstantStride,
                    &depthConstants, sizeof(depthConstants));
    }

    // M14：HZB 可视化的常量（当前查看的 mip + 总级数 + near/far）
    {
        struct HZBVisualizeConstants
        {
            std::uint32_t mipLevel;
            std::uint32_t mipCount;
            float nearPlane;
            float farPlane;
        };
        HZBVisualizeConstants hzbConstants = {};
        hzbConstants.mipLevel = m_hzbViewMip;
        hzbConstants.mipCount = m_hzb.GetMipCount();
        hzbConstants.nearPlane = m_nearPlane;
        hzbConstants.farPlane = m_farPlane;
        std::memcpy(constantBase +
                        static_cast<std::size_t>(kHZBVisualizeConstantSlot) * kConstantStride,
                    &hzbConstants, sizeof(hzbConstants));
    }

    frame.constantBuffer.Unmap(0, nullptr);

    // 6. 生成调试线框与统计文本的顶点，并上传到 GPU（属于 CPU 侧的 update 阶段）。
    //
    //    M15：遮挡剔除可视化需要先把 GPU 的可见列表读回来（一次独立提交），
    //    所以它必须排在 BuildDebugVisualization **之前** —— 后者要用到那份列表。
    //    未开启可视化时这个调用会立刻返回，不产生任何提交。
    BuildOcclusionDebugLines();
    ReadbackInstanceLOD(); // M16：为 LOD 着色准备每实例的级数

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

    // 8. 视口 / 裁剪矩形。
    const D3D12_VIEWPORT viewport = {
        0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f
    };
    const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissor);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(backBufferIndex) * m_rtvDescriptorSize;

    // 9. 清颜色。
    //
    //    注意**深度清屏不在这里** —— M13 把深度缓冲的所有权交给了 Depth Prepass，
    //    由那一遍负责清深度并写入最近表面的深度值。两个 Pass 共用同一个
    //    深度缓冲，清屏只应该发生一次。
    const float clearColor[4] = { 0.02f, 0.02f, 0.05f, 1.0f };
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    // 10. 设置管线状态。根签名 / 描述符堆整帧只设一次 ——
    //     这正是要凸显的结构：昂贵的状态放循环外，廉价的绑定放循环内。
    //
    //     M10 在图形管线之前插入了一个 Compute Pass（GPU 视锥剔除）。
    //     顺序上必须注意：compute 与 graphics 各自有自己的根签名/PSO 绑定，
    //     所以下面重新 SetGraphicsRootSignature + SetPipelineState 是必要的 ——
    //     不能假设图形状态还留在上一帧的设置上。
    ID3D12DescriptorHeap* const descriptorHeaps[] = { m_srvHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, descriptorHeaps);

    // M13：t0 —— 帧开始（在任何计算 Pass 之前）
    m_gpuProfiler.WriteTimestamp(m_commandList.Get(), m_frameIndex,
                               GPUProfiler::kSlotFrameBegin);

    // M15/M16：遮挡剔除是否生效（M16 的 LOD 选择需要提前知道这一点，
    // 因为两者目前不能叠加 —— 见 M16 Limitation）。
    const bool occlusionActive = m_occlusionEnabled && m_depthPrepassEnabled && totalCount > 0;

    // 10a. GPU 视锥剔除 + LOD 选择 + Stream Compaction：每线程一个实例，
    //      结果写进「压缩后的可见索引列表 + 计数器」。
    //      Dispatch 的线程组数 = ceil(instanceCount / 64)，见 GPUFrustumCuller。
    if (totalCount > 0)
    {
        // M16：剔除 CS 现在同时做 LOD 选择，所以要额外传 viewProj 与投影参数。
        const XMMATRIX cullProjection = m_camera.GetProjection();
        XMFLOAT4X4 cullViewProjF4x4;
        XMStoreFloat4x4(&cullViewProjF4x4, viewProj);

        // 遮挡剔除目前只处理单段列表，所以两者同时开启时把 LOD 退化为 1 级。
        const bool useLOD = m_lodEnabled && !occlusionActive;
        const std::uint32_t cullLODCount = useLOD ? m_lodCount : 1u;

        m_frustumCuller.Record(m_commandList.Get(), m_srvHeap.Get(), m_srvDescriptorSize,
                               kInstanceSrvSlot, kLODMetadataSrvSlot, m_visibleList,
                               m_lastFrustumPlanes, cullViewProjF4x4,
                               cullProjection.r[1].m128_f32[1],
                               useLOD ? m_lodBias : -1.0f, // 偏置 -1 等价于强制 LOD0
                               cullLODCount,
                               totalCount);
    }

    // 10b. GPU 命令生成：为**每个 LOD** 生成一条 DrawIndexed 间接命令。
    //
    //      它读 LOD 元数据 + 每级的实例计数器，写 indirect argument buffer。
    //      实际画多少实例、用哪一级几何，**两个决定都在 GPU 上**。
    if (totalCount > 0)
    {
        m_indirectCommands.Record(m_commandList.Get(), m_srvHeap.Get(), m_srvDescriptorSize,
                                  m_visibleList, kLODMetadataSrvSlot);
    }

    // 10c. 切到图形管线。两条路径各自的绑定完全不同，所以这里是分支点：
    //
    //      CPU-Driven：主根签名 + 网格 PSO，逐个实例 SetCBV + Draw（第 11 步的循环）
    //      GPU-Driven：独立的根签名 + PSO，然后**一次** ExecuteIndirect
    const D3D12_VERTEX_BUFFER_VIEW& vertexBufferView = m_stressMesh.GetVertexBufferView();
    const D3D12_INDEX_BUFFER_VIEW& indexBufferView = m_stressMesh.GetIndexBufferView();
    const UINT indexCount = m_stressMesh.GetIndexCount();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

    // M13：t1 —— GPU 剔除与命令生成结束（从这里开始才算图形 Pass）
    m_gpuProfiler.WriteTimestamp(m_commandList.Get(), m_frameIndex,
                               GPUProfiler::kSlotCullEnd);

    // 10d. **参数缓冲进入 INDIRECT_ARGUMENT**。
    //
    //      这一步必须在**任何** ExecuteIndirect 之前完成 —— 包括 Depth Prepass。
    //      上一步的命令生成 CS 刚把它当 UAV 写完，此刻它的状态是
    //      UNORDERED_ACCESS；而 ExecuteIndirect 要求它处于
    //      D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT（它告诉驱动这块内存
    //      接下来由**命令处理器**读取，而不是被着色器读写）。
    //
    //      放在这里而不是放在各自的绘制函数里，是因为 Depth Pass 与 Main Pass
    //      会**共用同一份命令**连续执行两次 —— 状态只需要转一次。
    m_indirectCommands.TransitionTo(m_commandList.Get(),
                                    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    m_visibleList.TransitionIndicesTo(m_commandList.Get(),
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // 10e. **M13：Depth Prepass** —— 只写深度的一遍，用可见实例的间接命令绘制。
    //
    //      它排在 Main Pass 之前，把最近表面的深度先写进深度缓冲。
    //      Main Pass 随后以 LESS_EQUAL + 不写深度的方式复用它，
    //      被遮挡的片元在**像素着色之前**就被 Early-Z 丢弃。
    if (m_depthPrepassEnabled && totalCount > 0)
    {
        RecordDepthPrepass();
    }
    else
    {
        // 关闭 prepass 时，深度缓冲仍然需要被清一次 ——
        // 否则上一帧的深度会残留下来，Main Pass 靠它做测试会大面积失败。
        TransitionDepthBuffer(D3D12_RESOURCE_STATE_DEPTH_WRITE);
        m_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        m_gpuProfiler.WriteTimestamp(m_commandList.Get(), m_frameIndex,
                                   GPUProfiler::kSlotDepthEnd);
    }

    // 10g. **M14：构建 HZB（层级深度金字塔）**。
    //
    //      它读上面那一遍写出的全分辨率深度，逐级 2x2 Max 归约到 1x1。
    //      整个金字塔**全部在 GPU 上生成** —— CPU 不参与、不回读任何一级。
    //
    //      为什么放在这里：HZB 描述的是「本帧的不透明几何深度」，
    //      所以必须在深度 Pass **之后**、Main Pass 之前（下一帧做遮挡剔除时
    //      用的就是这个金字塔）。
    RecordHZBBuild();

    // 10h. **M15：HZB 遮挡剔除**。
    //
    //      这是 M15 新增的一步：拿刚构建好的 HZB 对「视锥内候选」做保守遮挡测试，
    //      输出真正需要着色的可见列表。
    //
    //      注意它与 Depth Prepass 的分工：
    //        Depth Prepass 画的是**全部候选**（它的目的只是把深度铺出来）；
    //        遮挡剔除之后 Main Pass 只画**真正可见**的那些。
    //      所以省下的是 Main Pass 的绘制量（顶点处理 + 像素着色）。
    RecordOcclusionCulling(m_frameIndex);

    // 10i. **第二次命令生成**：这次为「遮挡剔除后的可见列表」写出命令。
    //
    //      它覆盖 10b 写出的那份命令 —— 这是安全的，因为命令缓冲是**顺序消费**的：
    //      Depth Prepass 用的那一份已经在 10e 执行完毕。
    if (occlusionActive)
    {
        m_indirectCommands.Record(m_commandList.Get(), m_srvHeap.Get(), m_srvDescriptorSize,
                                  m_occlusionCuller.GetVisibleList(), kLODMetadataSrvSlot);

        // 参数缓冲再次被 CS 当 UAV 写过，所以要再转一次 INDIRECT_ARGUMENT；
        // 可见列表也要重新变成 SRV 给顶点着色器读。
        m_indirectCommands.TransitionTo(m_commandList.Get(),
                                        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        m_occlusionCuller.GetVisibleList().TransitionIndicesTo(
            m_commandList.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // 10j. Main Pass 的渲染目标绑定（Depth Prepass 时绑定的是 0 个 RTV）。
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    if (m_renderMode == RenderMode::GpuDriven)
    {
        m_stats.gpuDriven = true;
        m_stats.submittedDrawCalls = 0;    // CPU 侧一次 draw 都没有提交
        m_stats.indirectExecuteCount = 1;  // 只有一次 ExecuteIndirect

        // Main Pass 用哪一份索引列表取决于遮挡剔除是否生效：
        //   遮挡剔除关闭 -> 视锥候选列表（**per-LOD 分段**，每级一个 SRV）
        //   遮挡剔除开启 -> occlusion 的可见列表（当前是单段，见下方 Limitation）
        //
        // M16 Limitation：per-LOD 的遮挡剔除尚未实现，所以两者同时开启时
        // 只有 LOD0 生效（lodCount 会被强制成 1，见 10a）。
        const UINT srvBaseSlot = occlusionActive
                                     ? m_occlusionCuller.GetVisibleList().GetIndicesSrvSlot()
                                     : kLODSegmentSrvSlot;
        const std::uint32_t drawLODCount = occlusionActive ? 1u : m_lodCount;
        RecordGpuDrivenDraw(submitCount, srvBaseSlot, drawLODCount);
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
    //     M13：深度可视化开着时就跳过场景叠加（它本来就是要「只看深度」）。
    if (m_debugLines.HasContent() && !m_depthVisualizeEnabled)
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

    // 12b. M13：深度可视化 —— 全屏三角形采样深度缓冲，覆盖整个画面。
    //      HZB 可视化开着时以它为准（两者都是全屏覆盖，同时开会互相覆盖）。
    if (m_depthVisualizeEnabled && !m_hzbVisualizeEnabled)
    {
        RecordDepthVisualization();
    }

    // 12c. M14：HZB mip 可视化 —— 任选一级金字塔放大铺满屏幕。
    if (m_hzbVisualizeEnabled)
    {
        RecordHZBVisualization();
    }

    // 12d. 时间戳：Main Pass 结束（含深度/HZB 可视化，如果开了的话）
    m_gpuProfiler.WriteTimestamp(m_commandList.Get(), m_frameIndex,
                                 GPUProfiler::kSlotMainEnd);

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

    // 14. M13：GPU 时间戳解析。
    //     ResolveQueryData 本身是一条命令，必须在 Close 之前记录。
    //     它把查询堆里的三个时间点拷进 readback 缓冲，CPU 在**若干帧之后**
    //     才去读那一帧的结果 —— 那时 GPU 早已写完，Map 不会阻塞。
    m_gpuProfiler.Resolve(m_commandList.Get(), m_frameIndex);

    // 15. 关闭命令列表并提交，Present，Signal。
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

    // M13：读回**若干帧之前**的时间戳（那时 GPU 早已完成，不会阻塞）。
    // 用 kFrameCount 作为延迟帧数，正好是「这个 frame slot 被复用」的时刻。
    {
        double cullMs = 0.0;
        double depthMs = 0.0;
        double mainMs = 0.0;
        // M15：遮挡剔除统计（同样延迟读回，不阻塞）
        m_occlusionStats = m_occlusionCuller.ReadbackStats(m_frameIndex);
        {
        }

        if (m_gpuProfiler.ReadbackCompletedFrame(m_frameIndex, cullMs, depthMs, mainMs))
        {
            constexpr double kSmoothing = 0.1;
            m_avgGpuCullMs = m_avgGpuCullMs * (1.0 - kSmoothing) + cullMs * kSmoothing;
            m_avgGpuDepthMs = m_avgGpuDepthMs * (1.0 - kSmoothing) + depthMs * kSmoothing;
            m_avgGpuMainMs = m_avgGpuMainMs * (1.0 - kSmoothing) + mainMs * kSmoothing;
        }
    }

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
                  << " prepass=" << (m_depthPrepassEnabled ? "on" : "off")
                  << " cpuDraws=" << m_stats.submittedDrawCalls
                  << " execIndirect=" << m_stats.indirectExecuteCount
                  << " culling=" << (m_cpuCullingEnabled ? "on" : "off")
                  << " cpuFrame=" << m_avgCpuFrameMs << "ms"
                  << " cull=" << m_avgCullMs << "ms"
                  << " update=" << m_avgUpdateMs << "ms"
                  << " record=" << m_avgRecordMs << "ms"
                  << " present=" << m_avgPresentMs << "ms"
                  << " gpuCull=" << m_avgGpuCullMs << "ms"
                  << " gpuDepth=" << m_avgGpuDepthMs << "ms"
                  << " gpuMain=" << m_avgGpuMainMs << "ms\n";
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
    // 深度模板：开启深度测试。
    //
    // M13：开了 Depth Prepass 之后，这一遍**复用** prepass 写好的深度：
    //   关闭深度写入 + 放宽成 LESS_EQUAL（两遍的深度值逐位一致，
    //   用严格的 LESS 会把自己刚写的深度判失败，画面会全空）。
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = m_depthPrepassEnabled
                                                ? D3D12_DEPTH_WRITE_MASK_ZERO
                                                : D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = m_depthPrepassEnabled
                                           ? D3D12_COMPARISON_FUNC_LESS_EQUAL
                                           : D3D12_COMPARISON_FUNC_LESS;
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

    // 4. SRV（M13）：把 D32_FLOAT 以 R32_FLOAT 暴露给着色器，用于深度可视化。
    //
    //    同一个资源可以同时有 DSV 与 SRV 两个视图，但**不能在同一次访问里
    //    同时使用** —— 所以采样深度之前必须把资源从 DEPTH_WRITE 转到
    //    PIXEL_SHADER_RESOURCE（见 Renderer::TransitionDepthBuffer）。
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.PlaneSlice = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE depthSrvHandle =
        m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    depthSrvHandle.ptr += static_cast<SIZE_T>(kDepthSrvSlot) * m_srvDescriptorSize;
    device->CreateShaderResourceView(m_depthBuffer.Get(), &srvDesc, depthSrvHandle);

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
    constexpr UINT kLastConstantSlot = kHZBVisualizeConstantSlot;
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

    // M16：用「细分立方体的 LOD 链」取代单一立方体。
    //
    // 各级几何被拼进**同一个**顶点/索引缓冲，用 (baseVertex, indexOffset)
    // 区分 —— 于是间接命令里的三个字段就足以选择几何，
    // 完全不需要切换缓冲绑定（见 MeshLOD.h 的说明）。
    if (!CreateLODChain(device, uploadList.Get(), stagingResources))
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
                                  kVisibleIndicesSrvSlot, kVisibleCountSrvSlot,
                                  m_lodCount)) // M16：按 LOD 分段
    {
        std::cerr << "[Renderer] Failed to create visible instance list.\n";
        return false;
    }
    if (!m_frustumCuller.Initialize(device, kMaxInstances, m_srvHeap.Get(),
                                    m_srvDescriptorSize, kInstanceLodUavSlot))
    {
        std::cerr << "[Renderer] Failed to create GPU frustum culler.\n";
        return false;
    }

    // ---- M12/M16：GPU-Driven 渲染所需的间接命令基础设施 ----
    // lodCount = 命令条数（每个 LOD 一条）
    if (!m_indirectCommands.Initialize(device, kMaxInstances, m_lodCount, m_srvHeap.Get(),
                                       m_srvDescriptorSize, kIndirectArgsUavSlot))
    {
        std::cerr << "[Renderer] Failed to create indirect draw commands.\n";
        return false;
    }

    // M16：为 per-LOD 索引列表建「每段一个 SRV」。
    // 顶点着色器因此完全不需要知道 LOD 的存在。
    CreateLODSegmentSrvs(device, m_visibleList);

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

    // 深度状态必须与 CPU-Driven 的网格 PSO 一致，否则切换模式时画面会变。
    //
    // M13：如果开了 Depth Prepass，这一遍**复用**前面写好的深度 ——
    //   把深度写入关掉、比较函数放宽成 LESS_EQUAL。
    //   用 LESS_EQUAL 而不是 LESS 的原因：两遍的 VS 完全相同，深度值逐位一致，
    //   严格的小于会把自己刚写进去的深度判为失败，整个画面变成空白。
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = m_depthPrepassEnabled
                                                ? D3D12_DEPTH_WRITE_MASK_ZERO
                                                : D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = m_depthPrepassEnabled
                                           ? D3D12_COMPARISON_FUNC_LESS_EQUAL
                                           : D3D12_COMPARISON_FUNC_LESS;
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

// ---------------------------------------------------------------------------
// M13：Depth Prepass PSO（只写深度，不输出颜色）
//
// 关键设置：
//   * NumRenderTargets = 0，RTVFormats[0] = UNKNOWN  ——  完全不绑 RTV。
//     这样驱动不需要分配任何颜色目标，像素阶段也只做深度测试/写入。
//   * PS = nullptr  ——  没有像素着色器。深度测试与写入属于**光栅化阶段**的
//     固定功能，不经过 PS，所以 depth-only 根本不需要它。
//   * DepthFunc = LESS + WRITE_ALL  ——  这一遍负责把最近表面的深度写进去。
//
// 顶点着色器与 GPU-Driven 主路径**完全相同**（同一个 .hlsl、同一套根签名），
// 因此两遍算出的深度值逐位一致 —— 这是 Main Pass 能用 LESS_EQUAL 通过的前提。
// ---------------------------------------------------------------------------
bool Renderer::CreateDepthOnlyPipelineState(ID3D12Device* device)
{
    std::string errorMsg;
    std::vector<std::uint8_t> vsBytecode = ShaderCompiler::Compile(
        L"MeshGPUDrivenVS.hlsl", L"main", L"vs_6_0", errorMsg);
    if (vsBytecode.empty())
    {
        std::cerr << "[Renderer] Depth-only VS compile failed:\n" << errorMsg << "\n";
        return false;
    }

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    // 复用 GPU-Driven 的根签名：两遍的 VS 是同一个，绑定需求完全一致
    desc.pRootSignature = m_gpuDrivenRootSignature.Get();
    desc.VS = { vsBytecode.data(), vsBytecode.size() };
    desc.PS = {}; // 没有像素着色器

    // 不写颜色，所以混合状态无关紧要（保持默认）
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
    desc.SampleMask = UINT_MAX;

    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // 背面剔除**必须与 Main Pass 完全一致**（这里复用同一个 MeshRasterizerState）。
    //
    // 如果两遍的剔除设置不同，被某一遍剔掉的三角形在另一遍仍然会被光栅化：
    // 深度缓冲里就会出现没有被 prepass 覆盖的区域 —— 表现为画面上出现
    // 一阵阵的深度空洞（本该被遮挡的片元因为没写深度而通过了测试）。
    desc.RasterizerState.CullMode = MeshRasterizerState().CullMode;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthClipEnable = TRUE;

    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DepthStencilState.StencilEnable = FALSE;

    desc.InputLayout = { inputLayout,
                         static_cast<UINT>(sizeof(inputLayout) / sizeof(inputLayout[0])) };
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    // -------------------------------------------------------------------------
    // NumRenderTargets = 0：真正的 depth-only，完全不绑定颜色目标。
    //
    // 一度改成过 NumRenderTargets = 1（保留一个 RTV，PS 仍为 null），
    // 理由是当时测到「无颜色输出」比「有一个 RTV」慢约 3 倍。
    // **那个读数后来被证实是测量环境问题**（显存吃紧时 GPU 会明显降速）。
    // 在干净的显存状态下重做 A/B：
    //
    //     100k 实例、92 万三角形、RTX 5070，各测 3 次：
    //       NumRenderTargets = 0  ->  depth 0.524 ms  main 0.810 ms  total 1.333 ms
    //       NumRenderTargets = 1  ->  depth 0.526 ms  main 0.816 ms  total 1.342 ms
    //
    //   两者**没有可测量的差异**，所以这里回到语义更纯粹的写法。
    //
    //   > 顺带记下一个方法学教训：GPU 计时对显存压力非常敏感。
    //   > 两次测量之间必须确认环境一致，否则会得出完全错误的「优化结论」——
    //   > 这正是本项目在 M13 踩过的坑（详见 docs/BENCHMARKS.md 第 10 节）。
    // -------------------------------------------------------------------------
    desc.NumRenderTargets = 0;
    desc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;

    return SUCCEEDED(device->CreateGraphicsPipelineState(&desc,
                                                         IID_PPV_ARGS(&m_depthOnlyPipelineState)));
}

// ---------------------------------------------------------------------------
// M13：深度可视化 PSO
//
// 全屏三角形（VS 用 SV_VertexID 现场生成，不需要顶点缓冲）+ 采样深度 SRV。
// 用**主根签名**（b0 = ObjectConstants，t0 走参数 1 的 SRV 表）——
// 它本身就是「一个 CBV + 一张 SRV 表」的形状，正好够用。
// ---------------------------------------------------------------------------
bool Renderer::CreateDepthVisualizePipelineState(ID3D12Device* device)
{
    std::string errorMsg;
    std::vector<std::uint8_t> vsBytecode = ShaderCompiler::Compile(
        L"DepthVisualizeVS.hlsl", L"main", L"vs_6_0", errorMsg);
    if (vsBytecode.empty())
    {
        std::cerr << "[Renderer] Depth-visualize VS compile failed:\n" << errorMsg << "\n";
        return false;
    }
    std::vector<std::uint8_t> psBytecode = ShaderCompiler::Compile(
        L"DepthVisualizePS.hlsl", L"main", L"ps_6_0", errorMsg);
    if (psBytecode.empty())
    {
        std::cerr << "[Renderer] Depth-visualize PS compile failed:\n" << errorMsg << "\n";
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_rootSignature.Get();
    desc.VS = { vsBytecode.data(), vsBytecode.size() };
    desc.PS = { psBytecode.data(), psBytecode.size() };

    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;

    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // 全屏三角形没有正反面
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthClipEnable = TRUE;

    // 覆盖整个屏幕，所以关闭深度测试
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilEnable = FALSE;

    // 没有顶点缓冲：所有输入由 SV_VertexID 生成
    desc.InputLayout = { nullptr, 0 };
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;

    return SUCCEEDED(device->CreateGraphicsPipelineState(
        &desc, IID_PPV_ARGS(&m_depthVisualizePipelineState)));
}

// ===========================================================================
// M14：HZB（Hierarchical Z-Buffer）
// ===========================================================================

bool Renderer::CreateHZBVisualizePipelineState(ID3D12Device* device)
{
    std::string errorMsg;
    std::vector<std::uint8_t> vsBytecode = ShaderCompiler::Compile(
        L"DepthVisualizeVS.hlsl", L"main", L"vs_6_0", errorMsg);
    if (vsBytecode.empty())
    {
        std::cerr << "[Renderer] HZB visualize VS compile failed:\n" << errorMsg << "\n";
        return false;
    }
    std::vector<std::uint8_t> psBytecode = ShaderCompiler::Compile(
        L"HZBVisualizePS.hlsl", L"main", L"ps_6_0", errorMsg);
    if (psBytecode.empty())
    {
        std::cerr << "[Renderer] HZB visualize PS compile failed:\n" << errorMsg << "\n";
        return false;
    }

    // 复用主根签名（b0 = CBV，t0 走参数 1 的 SRV 表，静态采样器 s0）——
    // 与深度可视化完全相同的形状。
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_rootSignature.Get();
    desc.VS = { vsBytecode.data(), vsBytecode.size() };
    desc.PS = { psBytecode.data(), psBytecode.size() };
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.InputLayout = { nullptr, 0 }; // 全屏三角形由 SV_VertexID 生成
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;

    return SUCCEEDED(device->CreateGraphicsPipelineState(
        &desc, IID_PPV_ARGS(&m_hzbVisualizePipelineState)));
}

void Renderer::RecordHZBBuild()
{
    if (!m_hzbEnabled || m_hzb.GetMipCount() == 0)
    {
        return;
    }

    // 深度缓冲这一次要被 CS **读取**（作为 HZB 第 0 级的源），
    // 所以必须离开 DEPTH_WRITE 状态 —— DSV 与 SRV 是同一资源的两个视图，
    // 不能在同一次访问里同时使用。
    TransitionDepthBuffer(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    m_hzb.Build(m_commandList.Get(), m_srvHeap.Get(), m_srvDescriptorSize, kDepthSrvSlot);

    // 构建完必须把深度缓冲**转回去**给后续的图形 Pass 用。
    //
    // 为什么：HZB 的 CS 把深度当作 SRV 读，所以上面转到了
    // NON_PIXEL_SHADER_RESOURCE。但 Main Pass 要把它绑定成 **DSV** 做深度测试，
    // 而 DSV 要求资源处于 DEPTH_WRITE 或 DEPTH_READ ——
    // 让一个处于 SRV 状态的纹理去当深度附件，是资源状态使用错误。
    //
    // 这里用 DEPTH_WRITE 而不是 DEPTH_READ，是因为 Main Pass 的 PSO 里
    // `DepthWriteMask = ZERO` 已经保证它不会真的写；用 DEPTH_WRITE 可以让
    // 「prepass 关闭」的路径（那一遍仍然要写深度）复用同一个状态，少一次转换。
    TransitionDepthBuffer(D3D12_RESOURCE_STATE_DEPTH_WRITE);

    // M14：在第 60 帧做一次 HZB 各级深度值的验证读回
    //（一次性、纯验证；金字塔的生成不依赖它）。
    if (m_frameCounter >= 60u && !m_hzbVerifyDone)
    {
        VerifyHZBLevels();
    }

    // M16：同一帧再做一次 LOD 分布验证（也是一次性）
    if (m_frameCounter >= 60u)
    {
        VerifyLODDistribution();
    }
}

// ---------------------------------------------------------------------------
// M14 验证：读回 HZB 每一级的一个采样值，检查它们是否满足 Max reduction 的
// 不变量。
//
// 要验证的性质：**HZB[i] <= HZB[i+1]**（单调不减）。
//   理由：第 i+1 级的一个像素覆盖第 i 级的 2x2 区域，
//         而它是那 4 个值的 max —— 所以一定 >= 其中任何一个。
//   若这个不等式被破坏，说明归约方向搞错了（用了 Min，或者采样点错位）。
//
// 同时检查所有值都落在 [0, 1]：这是传统 D3D 深度约定下 NDC z 的合法范围。
// 最后一级是 1x1，它的值就是**全屏最远**的深度 —— 可以直接和"场景最远
// 几何距离"作数量级对照。
//
// 注意：这是**一次性调试读回**，只用于验证生成结果；
// 金字塔本身的生成完全在 GPU 上完成，不依赖任何 CPU 回读。
// ---------------------------------------------------------------------------
bool Renderer::CreateHZBVerifyReadback(ID3D12Device* device)
{
    // 每一级拷 1 个像素（4 字节），行距按 256 对齐
    m_hzbVerifyRowPitch = 256;
    const UINT64 size = static_cast<UINT64>(m_hzbVerifyRowPitch) * HierarchicalZBuffer::kMaxMips;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    return SUCCEEDED(device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_hzbVerifyReadback)));
}

void Renderer::VerifyHZBLevels()
{
    if (m_hzbVerifyDone || m_hzb.GetMipCount() == 0 || m_hzbVerifyReadback == nullptr)
    {
        return;
    }
    m_hzbVerifyDone = true;

    // HZB 必须先变成拷贝源
    // （用独立命令列表做，理由见下面拷贝循环处的说明）

    // 每一级取中心像素（1x1 对齐到 mip 中心），拷到 readback buffer。
    //
    // **注意这里必须用一个独立、立即提交的命令列表**：
    // 本函数是在主命令列表**还在录制**的过程中被调用的，
    // 如果往那个列表里记录拷贝再去 Signal/Wait，GPU 根本还没收到任何命令 ——
    // 读回来只会是缓冲区的初值 0（本阶段实际踩到这个坑，第一次读回全 0）。
    ComPtr<ID3D12CommandAllocator> verifyAllocator;
    ComPtr<ID3D12GraphicsCommandList> verifyList;
    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&verifyAllocator))) ||
        FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           verifyAllocator.Get(), nullptr,
                                           IID_PPV_ARGS(&verifyList))))
    {
        std::cerr << "[HZB-Verify] failed to create verify command list\n";
        return;
    }

    m_hzb.TransitionTo(verifyList.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);

    for (UINT mip = 0; mip < m_hzb.GetMipCount(); ++mip)
    {
        const UINT w = m_hzb.GetMipWidth(mip);
        const UINT h = m_hzb.GetMipHeight(mip);
        const UINT x = w / 2;
        const UINT y = h / 2;

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = m_hzbVerifyReadback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = static_cast<UINT64>(mip) * m_hzbVerifyRowPitch;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
        dst.PlacedFootprint.Footprint.Width = 1;
        dst.PlacedFootprint.Footprint.Height = 1;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = m_hzbVerifyRowPitch;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = m_hzb.GetResource();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = mip;

        const D3D12_BOX box = { x, y, 0, x + 1, y + 1, 1 };
        verifyList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }

    m_hzb.TransitionTo(verifyList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    verifyList->Close();
    ID3D12CommandList* lists[] = { verifyList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    // 等这一批拷贝完成后再读
    ++m_fenceValue;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue)
    {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    const D3D12_RANGE range = { 0, static_cast<SIZE_T>(m_hzbVerifyRowPitch) *
                                       m_hzb.GetMipCount() };
    void* mapped = nullptr;
    if (FAILED(m_hzbVerifyReadback->Map(0, &range, &mapped)))
    {
        std::cerr << "[HZB-Verify] Map failed\n";
        return;
    }

    std::cout << "\n===== HZB Depth Value Validation (M14) =====\n";
    std::cout << "Reduction: MAX (D3D depth, near=0 far=1, larger = farther)\n\n";

    bool monotonic = true;
    bool inRange = true;
    float previous = -1.0f;
    for (UINT mip = 0; mip < m_hzb.GetMipCount(); ++mip)
    {
        const float* row = reinterpret_cast<const float*>(
            static_cast<const std::uint8_t*>(mapped) +
            static_cast<SIZE_T>(mip) * m_hzbVerifyRowPitch);
        const float value = row[0];

        if (value < 0.0f || value > 1.0f)
        {
            inRange = false;
        }
        if (mip > 0 && value < previous - 1e-6f)
        {
            monotonic = false;
        }

        std::printf("  mip %2u  %5ux%-4u  center depth = %.6f%s\n",
                    mip, m_hzb.GetMipWidth(mip), m_hzb.GetMipHeight(mip), value,
                    (mip > 0 && value > previous + 1e-6f) ? "   (increased -> Max picked up a farther pixel)" : "");
        previous = value;
    }

    m_hzbVerifyReadback->Unmap(0, nullptr);

    std::printf("\n  [1] all values in [0,1]           : %s\n", inRange ? "PASSED" : "FAILED");
    std::printf("  [2] monotonic non-decreasing      : %s\n",
                monotonic ? "PASSED (HZB[i] <= HZB[i+1], required by Max reduction)"
                          : "FAILED (reduction direction is wrong!)");
    std::printf("  [3] last mip is 1x1 (whole screen): %s\n",
                (m_hzb.GetMipWidth(m_hzb.GetMipCount() - 1) == 1 &&
                 m_hzb.GetMipHeight(m_hzb.GetMipCount() - 1) == 1) ? "PASSED" : "FAILED");
    std::printf("\n  => %s\n\n", (inRange && monotonic) ? "HZB VALIDATION PASSED" : "HZB VALIDATION FAILED");
}

// ===========================================================================
// M15：HZB 遮挡剔除
// ===========================================================================

void Renderer::RecordOcclusionCulling(UINT frameIndexForStats)
{
    if (!m_occlusionCuller.IsInitialized() || !m_hzbEnabled || !m_occlusionEnabled)
    {
        return;
    }

    // ---- 资源状态：三者这次都要被 CS **读取** ----
    //
    // 1) HZB：平时在 UNORDERED_ACCESS（供自己逐级写入），读之前转到 NON_PIXEL。
    //    这是同一个资源的「写视图」与「读视图」互斥问题（M13 已经解释过）。
    m_hzb.TransitionTo(m_commandList.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // 2) 候选索引列表与它的计数器：同样要变成 SRV。
    //    注意上一段（10b 的命令生成）刚把它们当 SRV 用过，所以这里多半是空操作；
    //    保留调用是为了让本函数单独看也成立。
    m_visibleList.TransitionIndicesTo(m_commandList.Get(),
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_visibleList.TransitionCountTo(m_commandList.Get(),
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // ---- 相机参数 ----
    //
    // proj00 / proj11 用来把世界空间半径换算成 NDC 半径。
    // 直接从投影矩阵取，避免再手写一套 FOV / aspect 的推导而与相机脱节。
    const XMMATRIX view = m_camera.GetView();
    const XMMATRIX projection = m_camera.GetProjection();
    const XMMATRIX viewProj = XMMatrixMultiply(view, projection);

    XMFLOAT4X4 viewProjFloat4x4;
    XMStoreFloat4x4(&viewProjFloat4x4, viewProj);

    const float proj00 = projection.r[0].m128_f32[0];
    const float proj11 = projection.r[1].m128_f32[1];

    m_occlusionCuller.Record(
        m_commandList.Get(), m_device.Get(), m_srvHeap.Get(), m_srvDescriptorSize,
        m_visibleList,
        m_instanceBuffer.GetResource(),
        m_hzb.GetResource(),
        m_hzb.GetMipCount(),
        m_hzb.GetMipWidth(0), m_hzb.GetMipHeight(0),
        viewProjFloat4x4, proj00, proj11,
        m_width, m_height, m_nearPlane, m_farPlane,
        m_occlusionDepthBias);

    // 统计拷进 readback 的环形缓冲（CPU 会在若干帧之后无阻塞地读它）
    m_occlusionCuller.ResolveStats(m_commandList.Get(), frameIndexForStats);

    // HZB 转回可写，供下一帧的 Build 使用
    m_hzb.TransitionTo(m_commandList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void Renderer::RecordHZBVisualization()
{
    if (m_hzbVisualizePipelineState == nullptr || m_hzb.GetMipCount() == 0)
    {
        return;
    }

    // 要查看哪一级由常量缓冲给出（见 Render() 里的 kHZBVisualizeConstantSlot），
    // Shader 用 SampleLevel 显式采样那一级。

    // HZB 从「CS 可写」切到「像素着色器可读」。
    // 注意这里**不需要**再插 UAV barrier：Build() 结尾已经插过一次。
    m_hzb.TransitionTo(m_commandList.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(m_swapChain->GetCurrentBackBufferIndex()) *
                     m_rtvDescriptorSize;
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->SetPipelineState(m_hzbVisualizePipelineState.Get());

    m_commandList->SetGraphicsRootConstantBufferView(
        0, m_frames[m_frameIndex].constantBuffer.GetGPUVirtualAddress() +
               static_cast<UINT64>(kHZBVisualizeConstantSlot) * kConstantStride);

    D3D12_GPU_DESCRIPTOR_HANDLE hzbSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    hzbSrv.ptr += static_cast<UINT64>(kHZBSrvSlot) * m_srvDescriptorSize;
    m_commandList->SetGraphicsRootDescriptorTable(1, hzbSrv);

    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->DrawInstanced(3, 1, 0, 0);

    // 切回 UAV，让下一帧的 Build() 可以直接继续写（省掉一次转换）。
    m_hzb.TransitionTo(m_commandList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void Renderer::TransitionDepthBuffer(D3D12_RESOURCE_STATES newState)
{
    if (m_depthState == newState || m_depthBuffer == nullptr)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_depthBuffer.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = m_depthState;
    barrier.Transition.StateAfter = newState;
    m_commandList->ResourceBarrier(1, &barrier);

    m_depthState = newState;
}

void Renderer::RecordDepthPrepass()
{
    // =========================================================================
    // Depth Prepass：只写深度、不输出颜色的一遍。
    //
    // 它复用 M12 建立的间接命令与压缩列表 —— 也就是说这一遍**同样**
    // 只提交一次 ExecuteIndirect，画多少个实例由 GPU 的可见数决定。
    // 换的只是 PSO：depth-only（无 PS、无 RTV）。
    //
    // 为什么值得单独一遍（详见 LEARNING_NOTES M13）：
    //   主 Pass 的像素着色器（纹理采样 + 光照）成本远高于深度测试。
    //   先跑一遍廉价的纯深度，主 Pass 就能靠 Early-Z 在**像素着色之前**
    //   丢弃被遮挡的片元 —— 用一份额外的几何开销换掉大量的像素开销。
    // =========================================================================
    if (m_depthOnlyPipelineState == nullptr)
    {
        return;
    }

    // ---- 1. 时间戳：Depth Pass 开始时 ----
    //   注意 EndQuery 记录的是「GPU 执行到这一点的时刻」，不是 CPU 发起时刻。
    // ---- 2. 深度缓冲必须可写（若上一帧末为了可视化把它转成了 SRV，这里要转回来）----
    TransitionDepthBuffer(D3D12_RESOURCE_STATE_DEPTH_WRITE);

    // ---- 3. 清深度 ----
    //   两个 Pass 共用同一个深度缓冲，所以清屏只在这里做一次。
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // ---- 4. 绑定 depth-only PSO 与**全部根参数** ----
    //
    // 注意：prepass 的 PSO 复用 GPU-Driven 的根签名（两遍的 VS 是同一个），
    // 所以必须像主 Pass 一样把三个根参数都绑好 —— 包括实例数据与可见索引。
    // 漏绑任何一个是未定义行为（驱动会沿用上一次的绑定，或读到无效描述符），
    // 表现为设备移除且没有任何可读的错误信息。
    m_commandList->SetGraphicsRootSignature(m_gpuDrivenRootSignature.Get());
    m_commandList->SetPipelineState(m_depthOnlyPipelineState.Get());

    m_commandList->SetGraphicsRootConstantBufferView(
        0, m_frames[m_frameIndex].constantBuffer.GetGPUVirtualAddress() +
               static_cast<UINT64>(kGlobalConstantSlot) * kConstantStride);

    D3D12_GPU_DESCRIPTOR_HANDLE instanceSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    instanceSrv.ptr += static_cast<UINT64>(kInstanceSrvSlot) * m_srvDescriptorSize;
    m_commandList->SetGraphicsRootDescriptorTable(1, instanceSrv);

    D3D12_GPU_DESCRIPTOR_HANDLE visibleSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    visibleSrv.ptr += static_cast<UINT64>(kVisibleIndicesSrvSlot) * m_srvDescriptorSize;
    m_commandList->SetGraphicsRootDescriptorTable(2, visibleSrv);

    D3D12_GPU_DESCRIPTOR_HANDLE materialSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    materialSrv.ptr += static_cast<UINT64>(m_stressMaterial.srvIndex) * m_srvDescriptorSize;
    m_commandList->SetGraphicsRootDescriptorTable(3, materialSrv);

    // 几何数据
    const D3D12_VERTEX_BUFFER_VIEW& vertexBufferView = m_stressMesh.GetVertexBufferView();
    const D3D12_INDEX_BUFFER_VIEW& indexBufferView = m_stressMesh.GetIndexBufferView();
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
    m_commandList->IASetIndexBuffer(&indexBufferView);

    // 不绑 RTV：必须与 PSO 的 NumRenderTargets = 0 一致 ——
    // depth-only 不需要颜色目标，OMSetRenderTargets 的数量传 0。
    m_commandList->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);

    // ---- 5. **每个 LOD 一次 ExecuteIndirect** ----
    //
    // 每条命令带自己的几何偏移（StartIndex / BaseVertex）与自己的实例数，
    // 所以这里必须逐级提交 —— ExecuteIndirect 一次只能表达「从第几条开始、
    // 连续几条命令」，而我们的命令确实就是连续排列的（第 lod 条在 lod*20）。
    //
    // 换段 SRV 的原因：顶点着色器的 SV_InstanceID 在每次 draw 内从 0 开始，
    // 而每条命令的 instanceCount 只是**该级**的数量，所以 SRV 必须指向该段的起点
    // （见 CreateLODSegmentSrvs 的说明）。
    for (std::uint32_t lod = 0; lod < m_lodCount; ++lod)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE segSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        segSrv.ptr += static_cast<UINT64>(kLODSegmentSrvSlot + lod) * m_srvDescriptorSize;
        m_commandList->SetGraphicsRootDescriptorTable(2, segSrv);

        m_commandList->ExecuteIndirect(
            m_indirectCommands.GetCommandSignature(),
            1, // 每次只提交这一级的命令
            m_indirectCommands.GetArgumentBuffer(),
            static_cast<UINT64>(lod) * IndirectDrawCommands::kDrawIndexedArgumentSize,
            nullptr, 0);
    }

    // ---- 6. 时间戳：Depth Pass 结束 ----
    m_gpuProfiler.WriteTimestamp(m_commandList.Get(), m_frameIndex,
                               GPUProfiler::kSlotDepthEnd);
}

void Renderer::RecordDepthVisualization()
{
    if (m_depthVisualizePipelineState == nullptr || m_depthBuffer == nullptr)
    {
        return;
    }

    // 深度缓冲这次要作为**纹理**被采样，必须离开 DEPTH_WRITE 状态。
    // DSV 与 SRV 是同一个资源的两个视图，不能在同一次访问里同时使用。
    TransitionDepthBuffer(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(m_swapChain->GetCurrentBackBufferIndex()) *
                     m_rtvDescriptorSize;
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->SetPipelineState(m_depthVisualizePipelineState.Get());

    // 根参数 0：CBV（放 near/far 与 1/分辨率）
    m_commandList->SetGraphicsRootConstantBufferView(
        0, m_frames[m_frameIndex].constantBuffer.GetGPUVirtualAddress() +
               static_cast<UINT64>(kDepthVisualizeConstantSlot) * kConstantStride);

    // 根参数 1：SRV 表 -> 深度纹理
    D3D12_GPU_DESCRIPTOR_HANDLE depthSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    depthSrv.ptr += static_cast<UINT64>(kDepthSrvSlot) * m_srvDescriptorSize;
    m_commandList->SetGraphicsRootDescriptorTable(1, depthSrv);

    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // 全屏三角形：3 个顶点，没有顶点缓冲
    m_commandList->DrawInstanced(3, 1, 0, 0);
}

void Renderer::RegenerateScene(std::uint32_t instanceCount){
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

    // 这条验证路径与 CPU 侧的视锥剔除逐实例对比，所以必须让 GPU 也只做**视锥**
    // 剔除 —— 把 LOD 强制成 1 级（lodBias = -1 等价于永远选 LOD0），
    // 否则输出列表会被 LOD 分段打散，两边无法直接比对。
    const XMFLOAT4X4 identityViewProj = {};
    m_frustumCuller.Record(list.Get(), m_srvHeap.Get(), m_srvDescriptorSize,
                           kInstanceSrvSlot, kLODMetadataSrvSlot, m_visibleList,
                           m_lastFrustumPlanes, identityViewProj, 0.0f, -1.0f, 1u,
                           instanceCount);
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

    // 顺带读回间接命令的前几条，确认 GPU 写出的命令内容与预期一致
    // （InstanceCount 应当等于 GPU 算出的可见数、StartInstanceLocation 为 0）。
    // 这是 --compare-cull / G 键验证流程的一部分，不是临时调试代码。
    constexpr UINT kCommandsToDump = 4;
    m_indirectCommands.DebugReadbackFirstCommands(
        m_device.Get(), m_commandQueue.Get(), m_fence.Get(), m_fenceEvent,
        m_fenceValue, kCommandsToDump);
}

void Renderer::RecordGpuDrivenDraw(std::uint32_t visibleCount, UINT segmentSrvBaseSlot,
                                   std::uint32_t lodCount)
{
    // visibleCount 不再参与提交 —— 每条命令的 InstanceCount 由 GPU 写入。
    // 参数保留是为了给调用方一个明确的语义：CPU **知道**可见数，但**不用**它。
    (void)visibleCount;

    // =========================================================================
    // GPU-Driven 的绘制记录：整段只有 N 次（N = LOD 级数）ExecuteIndirect，
    // 没有任何逐实例循环。
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

    // 要绘制的实例索引列表（顶点着色器用它把 SV_InstanceID 换算成真正的实例 ID）。
    //
    // M16 起这个列表是**按 LOD 分段**的，每段一个 SRV（见 CreateLODSegmentSrvs）。
    // 段 SRV 在下面的提交循环里逐级切换 —— 因为每条命令的 instanceCount
    // 只是该级的数量，而 SV_InstanceID 在每次 draw 内从 0 开始。
    D3D12_GPU_DESCRIPTOR_HANDLE materialSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    materialSrv.ptr += static_cast<UINT64>(m_stressMaterial.srvIndex) * m_srvDescriptorSize;
    m_commandList->SetGraphicsRootDescriptorTable(3, materialSrv);

    // 4. **状态转换**：参数缓冲从 UNORDERED_ACCESS 转到 INDIRECT_ARGUMENT。
    //
    //    ExecuteIndirect 要求参数缓冲处于 D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT ——
    //    它告诉驱动这块内存接下来由**命令处理器**读取，而不是被着色器读写。
    //    用错状态会读到陈旧的命令（驱动会认为不需要刷新相关缓存）。
    //
    //    注意：Depth Prepass 已经提前把它转到 INDIRECT_ARGUMENT 了，
    //    这里的调用是同状态空操作 —— 保留它是为了让本函数单独看也成立。
    m_indirectCommands.TransitionTo(m_commandList.Get(),
                                    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    // 5. **每个 LOD 一次 ExecuteIndirect**。
    //
    //    每条命令的 InstanceCount 都是 GPU 写进去的（该级的实例数），
    //    所以「画多少个实例」仍然完全由 GPU 决定；CPU 只决定「提交几条」，
    //    而那个数量就是 LOD 级数 —— 一个编译期常数。
    //
    //    M16 相对 M12 的退让：命令条数从常数 1 变成常数 N（N = LOD 级数）。
    //    这是「按实例选不同几何」无法回避的代价 —— 几何选择只能表达在命令里。
    for (std::uint32_t lod = 0; lod < lodCount; ++lod)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE segSrv = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        segSrv.ptr += static_cast<UINT64>(segmentSrvBaseSlot + lod) * m_srvDescriptorSize;
        m_commandList->SetGraphicsRootDescriptorTable(2, segSrv);

        m_commandList->ExecuteIndirect(
            m_indirectCommands.GetCommandSignature(),
            1, // 只提交这一级的命令
            m_indirectCommands.GetArgumentBuffer(),
            static_cast<UINT64>(lod) * IndirectDrawCommands::kDrawIndexedArgumentSize,
            nullptr, 0);
    }

    // 6. 转回 UNORDERED_ACCESS，让下一帧的命令生成 CS 可以直接继续写。
    m_indirectCommands.TransitionTo(m_commandList.Get(),
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// ---------------------------------------------------------------------------
// M15：遮挡剔除可视化 —— 计算「被 HZB 遮挡剔除」的实例集合
//
// 做法：**用 CPU 侧已有的视锥内集合作差**，不新增任何 GPU 资源 ——
//   候选（视锥内）= CPU 自己的剔除结果 m_visibleIndices（M10 已验证与 GPU 一致）
//   最终可见      = GPU 遮挡剔除输出的列表（延迟读回）
//   被遮挡剔除    = 前者 − 后者
//
// 读回是**独立提交 + 立即等待**的一次性拷贝。这不是每帧同步：
// 它只在可视化开启时执行，而且拷贝量有上限（kMaxReadback）。
// 正常运行时（K 关闭）这条路径完全不参与。
//
// 结果存进 m_occlusionDebugVisible（排序后的可见索引），
// 真正的画线由 UpdateDebugLines 里的双指针循环完成 —— 与视锥可视化共用一套遍历。
// ---------------------------------------------------------------------------
void Renderer::BuildOcclusionDebugLines()
{
    m_occlusionDebugVisible.clear();

    if (!m_occlusionVisualize || !m_occlusionEnabled || !m_occlusionCuller.IsInitialized())
    {
        return;
    }

    VisibleInstanceList& visibleList = m_occlusionCuller.GetVisibleList();
    if (m_occlusionDebugReadback == nullptr || visibleList.GetIndexBuffer() == nullptr)
    {
        return;
    }

    constexpr UINT kMaxReadback = 8192; // 只画前若干个，读回量可控
    const std::uint32_t capacity = visibleList.GetCapacity();
    const UINT copyCount = (capacity < kMaxReadback) ? capacity : kMaxReadback;

    // 主命令列表此刻还在录制中，所以这里必须用独立且立即提交的命令列表
    // （M14 的 HZB 验证踩过这个坑：往未提交的列表里记录拷贝再等待，读到的是初值）。
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&allocator))) ||
        FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        return;
    }

    list->CopyBufferRegion(m_occlusionDebugReadback.Get(), 0,
                           visibleList.GetIndexBuffer(), 0,
                           static_cast<UINT64>(copyCount) * sizeof(std::uint32_t));
    list->Close();

    ID3D12CommandList* lists[] = { list.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    ++m_fenceValue;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue)
    {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    const D3D12_RANGE range = { 0, static_cast<SIZE_T>(copyCount) * sizeof(std::uint32_t) };
    void* mapped = nullptr;
    if (FAILED(m_occlusionDebugReadback->Map(0, &range, &mapped)))
    {
        return;
    }

    const std::uint32_t* values = reinterpret_cast<const std::uint32_t*>(mapped);
    m_occlusionDebugVisible.assign(values, values + copyCount);
    m_occlusionDebugReadback->Unmap(0, nullptr);

    // 排序后就能像视锥可视化那样用双指针判断成员关系，避免每帧建哈希集合
    std::sort(m_occlusionDebugVisible.begin(), m_occlusionDebugVisible.end());
}

bool Renderer::CreateOcclusionDebugReadback(ID3D12Device* device)
{
    // 读回 GPU 可见列表的前若干项，用于遮挡剔除可视化
    constexpr UINT kMaxReadback = 8192;
    const UINT64 size = static_cast<UINT64>(kMaxReadback) * sizeof(std::uint32_t);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    return SUCCEEDED(device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_occlusionDebugReadback)));
}

// ===========================================================================
// M16：GPU-Driven LOD
// ===========================================================================

bool Renderer::CreateLODChain(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                              std::vector<ComPtr<ID3D12Resource>>& stagingOut)
{
    // -------------------------------------------------------------------------
    // 1. 生成 LOD 链几何
    //
    // 4 级细分立方体，每面细分数从 8 降到 1。所有级别的顶点与索引被**拼接**
    // 进同一个 MeshData —— 这样一次上传就能覆盖全部级别（见 MeshLOD.h）。
    // -------------------------------------------------------------------------
    const std::vector<std::uint32_t> segmentsPerLevel = { 8u, 4u, 2u, 1u };

    // 屏幕尺寸阈值：包围球投影直径占屏幕高度的比例。
    //
    //   >= 0.20          -> LOD0（每面 8x8，768 tri）
    //   [0.07, 0.20)     -> LOD1（4x4，192 tri）
    //   [0.025, 0.07)    -> LOD2（2x2，48 tri）
    //   < 0.025          -> LOD3（1x1，12 tri）
    //
    // 用「占屏幕比例」而不是像素数：阈值与分辨率无关。
    // 用「投影尺寸」而不是世界距离：阈值与 FOV 无关（详见 LEARNING_NOTES M16 Q1）。
    const std::vector<float> thresholds = { 0.20f, 0.07f, 0.025f };

    MeshData lodData = BuildSubdividedCubeLODChain(segmentsPerLevel, thresholds, m_lodRanges);
    m_lodCount = static_cast<std::uint32_t>(m_lodRanges.size());

    if (!m_stressMesh.Initialize(device, cmd, lodData, stagingOut))
    {
        std::cerr << "[Renderer] Failed to create LOD chain mesh.\n";
        return false;
    }

    std::cout << "[Renderer] LOD chain: " << m_lodCount << " levels, "
              << lodData.vertices.size() << " vertices / " << lodData.indices.size()
              << " indices total\n";
    for (std::uint32_t i = 0; i < m_lodCount; ++i)
    {
        std::cout << "        LOD" << i << ": indexOffset=" << m_lodRanges[i].indexOffset
                  << " indexCount=" << m_lodRanges[i].indexCount
                  << " baseVertex=" << m_lodRanges[i].baseVertex
                  << " tris=" << m_lodRanges[i].triangleCount
                  << " threshold=" << m_lodRanges[i].screenSizeThreshold << "\n";
    }

    // -------------------------------------------------------------------------
    // 2. 把 LOD 元数据上传到 GPU（StructuredBuffer<MeshLODRange>，stride 32）
    //
    // 它必须常驻显存：剔除 CS 每帧都要读它来算「该用哪一级」，
    // 而 CPU 每帧都不能参与 —— 这正是「LOD Selection 在 GPU 完成」的前提。
    // -------------------------------------------------------------------------
    const UINT64 metadataBytes = static_cast<UINT64>(m_lodRanges.size()) * sizeof(MeshLODRange);

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = metadataBytes;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> staging;
    if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&staging))))
    {
        std::cerr << "[Renderer] Failed to create LOD metadata staging buffer.\n";
        return false;
    }

    {
        void* mapped = nullptr;
        if (FAILED(staging->Map(0, nullptr, &mapped)))
        {
            return false;
        }
        std::memcpy(mapped, m_lodRanges.data(), static_cast<std::size_t>(metadataBytes));
        staging->Unmap(0, nullptr);
    }

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    // Buffer 的 InitialState 会被运行时忽略，一律写 COMMON，
    // 然后用显式 barrier 转到 COPY_DEST（否则 Debug Layer 报 ID=1328）。
    if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&m_lodMetadataBuffer))))
    {
        std::cerr << "[Renderer] Failed to create LOD metadata buffer.\n";
        return false;
    }

    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_lodMetadataBuffer.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cmd->ResourceBarrier(1, &barrier);
    }

    cmd->CopyBufferRegion(m_lodMetadataBuffer.Get(), 0, staging.Get(), 0, metadataBytes);

    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_lodMetadataBuffer.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cmd->ResourceBarrier(1, &barrier);
    }

    // staging 必须活到 GPU 执行完这批上传命令之后
    stagingOut.push_back(staging);

    // SRV：StructuredBuffer<MeshLODRange>
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN; // StructuredBuffer 必须用 UNKNOWN
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = m_lodCount;
    srvDesc.Buffer.StructureByteStride = sizeof(MeshLODRange);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(kLODMetadataSrvSlot) * m_srvDescriptorSize;
    device->CreateShaderResourceView(m_lodMetadataBuffer.Get(), &srvDesc, handle);

    return true;
}

void Renderer::CreateLODSegmentSrvs(ID3D12Device* device, VisibleInstanceList& list)
{
    // 为 per-LOD 索引列表的每一段建一个 SRV。
    //
    // 关键：顶点着色器用 SV_InstanceID（0..instanceCount-1）索引 SRV，
    // 而每条命令的 instanceCount 只是**该级**的数量。所以 SRV 的
    // FirstElement 必须指向该段的起点，SV_InstanceID 才能正确落在段内。
    //
    // 这样做的好处是**顶点着色器完全不需要知道 LOD 的存在** ——
    // 它看到的永远是一个从 0 开始的紧凑列表，与 M12 的行为一致。
    for (std::uint32_t lod = 0; lod < m_lodCount; ++lod)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = list.GetSegmentOffset(lod);
        srvDesc.Buffer.NumElements = list.GetCapacity();
        srvDesc.Buffer.StructureByteStride = sizeof(std::uint32_t);
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(kLODSegmentSrvSlot + lod) * m_srvDescriptorSize;
        device->CreateShaderResourceView(list.GetIndexBuffer(), &srvDesc, handle);
    }
}

// ---------------------------------------------------------------------------
// M16 验证：读回 per-LOD 的实例计数器，确认 LOD 选择真的在 GPU 上按尺寸发生了。
//
// 这一次性读回是**唯一**能证明「LOD 在切换」的手段 —— 光看画面无法区分
// 「几何变粗了」和「本来就长这样」。
//
// 同时给出「实际渲染三角形数」，这是本阶段要统计的核心指标：
//     renderedTris = Σ lodCounts[lod] × lodRanges[lod].triangleCount
// 它直接反映 LOD 的价值 —— 如果 LOD 全部落在 0 级，三角形数会远高于预期。
// ---------------------------------------------------------------------------
void Renderer::VerifyLODDistribution()
{
    if (m_lodVerifyDone || m_lodCount == 0 || m_occlusionDebugReadback == nullptr)
    {
        return;
    }
    // 只有「GPU-Driven 且遮挡剔除关闭」时 LOD 才真正生效
    //（否则 lodCount 会被强制成 1，per-LOD 计数器里只有第 0 段有意义）。
    // 在别的模式下验证不仅无意义，而且计数器压根没被写过。
    if (m_renderMode != RenderMode::GpuDriven || m_occlusionEnabled)
    {
        m_lodVerifyDone = true;
        return;
    }
    m_lodVerifyDone = true;

    // 计数器缓冲是 DEFAULT heap + UAV，先转到 COPY_SOURCE 再拷进 readback。
    // 这里用独立且立即提交的命令列表（主列表此刻还在录制中）。
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&allocator))) ||
        FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        return;
    }

    m_visibleList.TransitionCountTo(list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    const UINT countBytes = m_lodCount * static_cast<UINT>(sizeof(std::uint32_t));
    list->CopyBufferRegion(m_occlusionDebugReadback.Get(), 0,
                           m_visibleList.GetCountBuffer(), 0, countBytes);
    m_visibleList.TransitionCountTo(list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->Close();

    ID3D12CommandList* lists[] = { list.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    ++m_fenceValue;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue)
    {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    const D3D12_RANGE range = { 0, countBytes };
    void* mapped = nullptr;
    if (FAILED(m_occlusionDebugReadback->Map(0, &range, &mapped)))
    {
        return;
    }
    const std::uint32_t* counts = reinterpret_cast<const std::uint32_t*>(mapped);

    std::uint64_t renderedTriangles = 0;
    std::uint32_t totalInstances = 0;
    std::cout << "\n===== GPU LOD Selection Validation (M16) =====\n";
    std::cout << "LOD levels: " << m_lodCount
              << "  (selection performed entirely on the GPU)\n\n";

    for (std::uint32_t lod = 0; lod < m_lodCount; ++lod)
    {
        const std::uint32_t count = counts[lod];
        const std::uint32_t tris = m_lodRanges[lod].triangleCount;
        renderedTriangles += static_cast<std::uint64_t>(count) * tris;
        totalInstances += count;

        std::printf("  LOD%u  instances=%6u  tris/instance=%4u  -> %8llu tris   "
                    "(screen size >= %.4f)\n",
                    lod, count, tris,
                    static_cast<unsigned long long>(count) * tris,
                    m_lodRanges[lod].screenSizeThreshold);
    }

    // 如果全部落在 LOD0，说明尺寸判据没有生效 —— 这是个强判据
    const bool distributed = (counts[0] < totalInstances) || (m_lodCount == 1);
    const std::uint64_t naiveTriangles =
        static_cast<std::uint64_t>(totalInstances) * m_lodRanges[0].triangleCount;

    std::printf("\n  total instances (frustum-visible) : %u\n", totalInstances);
    std::printf("  rendered triangles (with LOD)     : %llu\n",
                static_cast<unsigned long long>(renderedTriangles));
    std::printf("  triangles if all LOD0             : %llu\n",
                static_cast<unsigned long long>(naiveTriangles));
    if (naiveTriangles > 0)
    {
        std::printf("  triangle reduction from LOD       : %.1f%%\n",
                    100.0 * (1.0 - static_cast<double>(renderedTriangles) /
                                       static_cast<double>(naiveTriangles)));
    }
    std::printf("\n  [1] LOD selection distributed across levels : %s\n",
                distributed ? "PASSED" : "FAILED (everything is LOD0 - size test not working)");
    std::printf("\n  => %s\n\n", distributed ? "LOD VALIDATION PASSED" : "LOD VALIDATION FAILED");

    m_lodTriangleBudget = static_cast<std::uint32_t>(renderedTriangles);
    // 同时缓存每级数量供屏幕统计使用（避免每帧再读一次 GPU）
    for (std::uint32_t lod = 0; lod < 8u; ++lod)
    {
        m_lodCounts[lod] = (lod < m_lodCount) ? counts[lod] : 0u;
    }

    m_occlusionDebugReadback->Unmap(0, nullptr);
}

// ---------------------------------------------------------------------------
// M16 可视化：把每实例的 LOD 读回 CPU，供调试视图按级着色。
//
// 这是**延迟 + 低频**的：只在 debug viz 打开时、每 30 帧读一次，
// 而且读回量按实例数上限截断。理由与 M15 的可视化一致 ——
// 调试可视化可以有一点滞后，但不能每帧打断 GPU 流水线。
//
// 着色约定（任务要求）：
//     LOD0 = 红   LOD1 = 黄   LOD2 = 绿   LOD3 = 蓝
// ---------------------------------------------------------------------------
void Renderer::ReadbackInstanceLOD()
{
    if (m_debugViewMode <= 0 || !m_lodEnabled || m_lodCount <= 1)
    {
        return;
    }
    // 与 VerifyLODDistribution 同样的前提：LOD 只在 GPU-Driven 且无遮挡剔除时生效
    if (m_renderMode != RenderMode::GpuDriven || m_occlusionEnabled)
    {
        return;
    }

    // 每 30 帧读一次就足够看清切换（LOD 变化本来就是低频事件）
    if ((m_lodReadbackCounter++ % 30u) != 0u)
    {
        return;
    }

    ID3D12Resource* lodBuffer = m_frustumCuller.GetInstanceLODBuffer();
    if (lodBuffer == nullptr || m_occlusionDebugReadback == nullptr)
    {
        return;
    }

    // readback 缓冲是 8192 个 uint，所以最多读这么多实例
    constexpr UINT kMaxLODReadback = 8192;
    const UINT instanceCount = m_instanceBuffer.GetInstanceCount();
    const UINT copyCount = (instanceCount < kMaxLODReadback) ? instanceCount : kMaxLODReadback;
    if (copyCount == 0)
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
        return;
    }

    // 该缓冲常驻 UNORDERED_ACCESS（CS 每帧写），读之前转到 COPY_SOURCE
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = lodBuffer;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
    }

    list->CopyBufferRegion(m_occlusionDebugReadback.Get(), 0, lodBuffer, 0,
                           static_cast<UINT64>(copyCount) * sizeof(std::uint32_t));

    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = lodBuffer;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        list->ResourceBarrier(1, &barrier);
    }

    list->Close();
    ID3D12CommandList* lists[] = { list.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    ++m_fenceValue;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue)
    {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    const D3D12_RANGE range = { 0, static_cast<SIZE_T>(copyCount) * sizeof(std::uint32_t) };
    void* mapped = nullptr;
    if (FAILED(m_occlusionDebugReadback->Map(0, &range, &mapped)))
    {
        return;
    }

    const std::uint32_t* values = reinterpret_cast<const std::uint32_t*>(mapped);
    m_instanceLODCache.assign(values, values + copyCount);
    m_occlusionDebugReadback->Unmap(0, nullptr);
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

    // ---- M13：切换 Depth Prepass ----
    // Main Pass 的深度状态烘焙在 PSO 里，所以这里只置位，
    // 重建放到下一帧 WaitForGpu 之后（那时旧 PSO 已经不被 GPU 使用）。
    case 'P':
        m_depthPrepassEnabled = !m_depthPrepassEnabled;
        m_pipelineStateRebuildRequested = true;
        std::cout << "[Renderer] Depth prepass: "
                  << (m_depthPrepassEnabled ? "ON (main pass reuses depth, LESS_EQUAL)"
                                            : "OFF (single pass, LESS + write)")
                  << "\n";
        break;

    // ---- M13：深度缓冲可视化 ----
    case 'B':
        m_depthVisualizeEnabled = !m_depthVisualizeEnabled;
        std::cout << "[Renderer] Depth visualization: "
                  << (m_depthVisualizeEnabled ? "ON" : "OFF") << "\n";
        break;

    // ---- M14：HZB 构建开关 ----
    case 'H':
        m_hzbEnabled = !m_hzbEnabled;
        std::cout << "[Renderer] HZB build: " << (m_hzbEnabled ? "ON" : "OFF") << "\n";
        break;

    // ---- M16：GPU-Driven LOD 开关 ----
    case 'L':
        m_lodEnabled = !m_lodEnabled;
        std::cout << "[Renderer] GPU LOD selection: "
                  << (m_lodEnabled ? "ON" : "OFF (all instances use LOD0)") << "\n";
        break;

    // ---- M16：运行时调整 LOD 阈值（通过全局偏置）----
    //
    // 偏置是**加到 screenSize 上**的，语义等价于「整体平移所有阈值」：
    //   偏置 > 0  -> screenSize 变大 -> 更容易满足高精度阈值 -> 偏向细 LOD
    //   偏置 < 0  -> 偏向粗 LOD
    //
    // 这是验证「阈值确实在驱动切换」最直接的手段：
    // 配合 --debug-viz 2 的四色视图，连续按键应看到 LOD 边界整体推移。
    case VK_OEM_COMMA: // ',' 更粗
        m_lodBias -= 0.02f;
        if (m_lodBias < -0.20f) { m_lodBias = -0.20f; }
        std::cout << "[Renderer] LOD bias = " << m_lodBias
                  << "  (negative = coarser LODs)\n";
        break;
    case VK_OEM_PERIOD: // '.' 更细
        m_lodBias += 0.02f;
        if (m_lodBias > 0.20f) { m_lodBias = 0.20f; }
        std::cout << "[Renderer] LOD bias = " << m_lodBias
                  << "  (positive = finer LODs)\n";
        break;

    // ---- M15：HZB 遮挡剔除开关 ----
    case 'O':
        m_occlusionEnabled = !m_occlusionEnabled;
        std::cout << "[Renderer] HZB occlusion culling: "
                  << (m_occlusionEnabled ? "ON" : "OFF (draw all frustum-visible)")
                  << "\n";
        break;

    // ---- M15：遮挡剔除可视化 ----
    case 'K':
        m_occlusionVisualize = !m_occlusionVisualize;
        if (m_occlusionVisualize && m_debugViewMode < 2)
        {
            m_debugViewMode = 2; // 需要包围球这一层才能标注
        }
        std::cout << "[Renderer] Occlusion culled visualization: "
                  << (m_occlusionVisualize
                          ? "ON (yellow = frustum-visible but occlusion-culled)"
                          : "OFF")
                  << "\n";
        break;

    // ---- M14：HZB mip 可视化 ----
    case 'N':
        m_hzbVisualizeEnabled = !m_hzbVisualizeEnabled;
        std::cout << "[Renderer] HZB visualization: "
                  << (m_hzbVisualizeEnabled ? "ON" : "OFF")
                  << " (viewing mip " << m_hzbViewMip << "/"
                  << (m_hzb.GetMipCount() > 0 ? m_hzb.GetMipCount() - 1 : 0) << ")\n";
        break;

    // ---- M14：切换要查看的 HZB mip 级 ----
    // '[' 往更细的一级走，']' 往更粗的一级走。
    case VK_OEM_4: // '['
        if (m_hzbViewMip > 0)
        {
            --m_hzbViewMip;
        }
        std::cout << "[Renderer] HZB view mip = " << m_hzbViewMip << " ("
                  << m_hzb.GetMipWidth(m_hzbViewMip) << "x"
                  << m_hzb.GetMipHeight(m_hzbViewMip) << ")\n";
        break;
    case VK_OEM_6: // ']'
        if (m_hzb.GetMipCount() > 0 && m_hzbViewMip + 1 < m_hzb.GetMipCount())
        {
            ++m_hzbViewMip;
        }
        std::cout << "[Renderer] HZB view mip = " << m_hzbViewMip << " ("
                  << m_hzb.GetMipWidth(m_hzbViewMip) << "x"
                  << m_hzb.GetMipHeight(m_hzbViewMip) << ")\n";
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
        y += 18.0f;

        // M13：Depth Prepass 状态与 GPU 侧耗时（Timestamp Query 测得）
        std::snprintf(line, sizeof(line), "Prepass   : %s   [P] toggle",
                      m_depthPrepassEnabled ? "ON" : "OFF");
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        std::snprintf(line, sizeof(line), "GPU cull  : %.3f ms", m_avgGpuCullMs);
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        std::snprintf(line, sizeof(line), "GPU depth : %.3f ms", m_avgGpuDepthMs);
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        std::snprintf(line, sizeof(line), "GPU main  : %.3f ms", m_avgGpuMainMs);
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        std::snprintf(line, sizeof(line), "Depth viz : %s   [B] toggle",
                      m_depthVisualizeEnabled ? "ON" : "OFF");
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        // M14：HZB 金字塔
        std::snprintf(line, sizeof(line), "HZB       : %s   %u mips  [H] toggle",
                      m_hzbEnabled ? "ON " : "OFF", m_hzb.GetMipCount());
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        if (m_hzb.GetMipCount() > 0)
        {
            std::snprintf(line, sizeof(line), "HZB view  : mip %u/%u  %ux%u   [N] viz  [ / ] level",
                          m_hzbViewMip, m_hzb.GetMipCount() - 1,
                          m_hzb.GetMipWidth(m_hzbViewMip), m_hzb.GetMipHeight(m_hzbViewMip));
            m_debugText.AddText(x, y, 1.0f, line);
            y += 18.0f;
        }

        // ---- M15：Frustum 与 Occlusion 剔除**分开统计** ----
        //
        // 这两个数字必须分开显示，否则无法判断「剔除到底发生在哪一步」：
        //   实例总数 ──视锥剔除──▶ 候选数 ──遮挡剔除──▶ 最终可见数
        //
        // 注意它来自**延迟若干帧的 GPU 读回**（不阻塞），所以数值比画面略有滞后。
        // frustumCulled 不需要额外统计 —— CPU 用「总数 − 候选数」直接算。
        {
            const std::uint32_t candidates = m_occlusionStats.valid
                                                 ? m_occlusionStats.candidateCount
                                                 : m_stats.visibleInstances;
            const std::uint32_t occluded = m_occlusionStats.valid
                                               ? m_occlusionStats.occludedCount
                                               : 0u;
            const std::uint32_t total = m_stats.totalInstances;
            const std::uint32_t frustumCulled = (total > candidates) ? (total - candidates) : 0u;
            const std::uint32_t finalVisible =
                (candidates > occluded) ? (candidates - occluded) : 0u;

            std::snprintf(line, sizeof(line), "Occlusion : %s   [O] toggle",
                          m_occlusionEnabled ? "ON" : "OFF");
            m_debugText.AddText(x, y, 1.0f, line);
            y += 18.0f;

            std::snprintf(line, sizeof(line), "  frustum : %u culled (%.1f%%)", frustumCulled,
                          total ? (100.0f * static_cast<float>(frustumCulled) /
                                   static_cast<float>(total)) : 0.0f);
            m_debugText.AddText(x, y, 1.0f, line);
            y += 18.0f;

            std::snprintf(line, sizeof(line), "  occluded: %u of %u (%.1f%%)", occluded,
                          candidates,
                          candidates ? (100.0f * static_cast<float>(occluded) /
                                        static_cast<float>(candidates)) : 0.0f);
            m_debugText.AddText(x, y, 1.0f, line);
            y += 18.0f;

            std::snprintf(line, sizeof(line), "  visible : %u   (conservative pass %u)",
                          finalVisible,
                          m_occlusionStats.valid ? m_occlusionStats.conservativePassCount : 0u);
            m_debugText.AddText(x, y, 1.0f, line);
            y += 18.0f;
        }

        // ---- M16：GPU-Driven LOD 的每级使用数量 ----
        //
        // 数据来自帧 60 的一次性读回（VerifyLODDistribution），
        // 所以它在整个运行期间是**静态**的 —— 改相机后需要重启才能刷新。
        // 这是刻意的：持续读回会打断 GPU 流水线，而每级数量的用途是验收而非实时监控。
        std::snprintf(line, sizeof(line), "GPU LOD   : %s   %u levels  [L] toggle",
                      m_lodEnabled ? "ON " : "OFF", m_lodCount);
        m_debugText.AddText(x, y, 1.0f, line);
        y += 18.0f;

        if (m_lodCount > 1 && m_lodTriangleBudget > 0)
        {
            std::snprintf(line, sizeof(line), "  LOD0..3 : %u / %u / %u / %u",
                          m_lodCounts[0], m_lodCounts[1], m_lodCounts[2], m_lodCounts[3]);
            m_debugText.AddText(x, y, 1.0f, line);
            y += 18.0f;

            const std::uint64_t naive =
                static_cast<std::uint64_t>(m_lodCounts[0] + m_lodCounts[1] +
                                           m_lodCounts[2] + m_lodCounts[3]) *
                768ull;
            std::snprintf(line, sizeof(line), "  tris    : %u  (%.0f%% saved)",
                          m_lodTriangleBudget,
                          naive ? (100.0 * (1.0 - static_cast<double>(m_lodTriangleBudget) /
                                                     static_cast<double>(naive)))
                                : 0.0);
            m_debugText.AddText(x, y, 1.0f, line);
            y += 18.0f;
        }
        y += 8.0f;

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

        m_debugText.AddText(x, y, 1.0f, "[L] LOD  [,][.] LOD bias  [O] occlusion  [K] culled viz  [M] mode");
        y += 18.0f;

        m_debugText.AddText(x, y, 1.0f, "[G] compare  [T] validate  [P] prepass  [B] depth viz  [H] HZB  [N] viz");
        y += 18.0f;

        m_debugText.AddText(x, y, 1.0f, "[1][2][3] count  [C] culling  [V] viz  [Space] orbit  [R] reset");
    };

    // 背景条：实例场景里模型很密集，纯文字会淹没在几何细节中，
    // 铺一层半透明黑底后统计信息在任何视角下都清晰。
    m_debugText.SetColor(0.0f, 0.0f, 0.0f, 0.55f);
    m_debugText.AddRect(4.0f, 4.0f, 680.0f, 780.0f);

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
    // M15：第三种颜色 —— 通过视锥测试、但被 HZB **遮挡剔除**掉了。
    // 这是本阶段最值得肉眼确认的一类：它们**不应该**出现在画面上，
    // 但它们的包围球仍然画出来，用来核对"剔掉的是不是真的看不见"。
    const XMFLOAT4 occlusionCulledColor = { 1.0f, 0.85f, 0.15f, 1.0f };

    // 可见集合是有序的，用双指针遍历避免每帧构造哈希集合
    std::size_t visibleCursor = 0;
    std::size_t gpuVisibleCursor = 0;
    std::size_t drawnSpheres = 0;

    for (std::size_t i = 0; i < total && drawnSpheres < kMaxSpheresToDraw; i += step)
    {
        while (visibleCursor < visibleIndices.size() && visibleIndices[visibleCursor] < i)
        {
            ++visibleCursor;
        }
        const bool visible = (visibleCursor < visibleIndices.size() &&
                              visibleIndices[visibleCursor] == i);

        XMFLOAT4 color = visible ? visibleColor : culledColor;

        // M15：在「视锥内」的实例里，再区分出「被遮挡剔除」的那些
        if (m_occlusionVisualize && visible)
        {
            while (gpuVisibleCursor < m_occlusionDebugVisible.size() &&
                   m_occlusionDebugVisible[gpuVisibleCursor] < i)
            {
                ++gpuVisibleCursor;
            }
            const bool gpuVisible = (gpuVisibleCursor < m_occlusionDebugVisible.size() &&
                                     m_occlusionDebugVisible[gpuVisibleCursor] == i);
            if (!gpuVisible)
            {
                color = occlusionCulledColor; // 视锥内 + 被遮挡剔除
            }
        }

        m_debugLines.AddSphere(instances[i].boundingSphere, color, 8);
        ++drawnSpheres;
    }

    // -------------------------------------------------------------------------
    // M16：按所选 LOD 着色（任务要求的可视化）
    //
    //   颜色直接来自剔除 CS 写下的 gInstanceLOD，所以它反映的是
    //   **GPU 的真实决定**，而不是 CPU 的猜测 —— 这是验证
    //   「LOD Selection 确实在 GPU 完成」最直观的手段。
    //
    //   着色约定：LOD0 = 红 / LOD1 = 黄 / LOD2 = 绿 / LOD3 = 蓝
    // -------------------------------------------------------------------------
    if (m_lodEnabled && m_lodCount > 1 && !m_instanceLODCache.empty())
    {
        static const XMFLOAT4 kLODColors[8] = {
            { 1.00f, 0.15f, 0.15f, 1.0f }, // LOD0 红
            { 1.00f, 0.90f, 0.15f, 1.0f }, // LOD1 黄
            { 0.20f, 1.00f, 0.25f, 1.0f }, // LOD2 绿
            { 0.25f, 0.45f, 1.00f, 1.0f }, // LOD3 蓝
            { 1.00f, 0.20f, 1.00f, 1.0f },
            { 0.20f, 1.00f, 1.00f, 1.0f },
            { 1.00f, 0.55f, 0.10f, 1.0f },
            { 0.60f, 0.60f, 0.60f, 1.0f },
        };

        for (std::size_t i = 0; i < total && i < m_instanceLODCache.size(); i += step)
        {
            const std::uint32_t lod = m_instanceLODCache[i];
            if (lod >= m_lodCount || lod >= 8u)
            {
                continue;
            }
            m_debugLines.AddSphere(instances[i].boundingSphere, kLODColors[lod], 12);
        }
    }

    m_debugLines.End();
}

