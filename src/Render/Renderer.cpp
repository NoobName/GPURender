#include "Render/Renderer.h"

#include "Asset/ObjLoader.h"
#include "Asset/TgaLoader.h"
#include "Render/ShaderCompiler.h"
#include "Render/UploadHelper.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstdint>
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

// 每个对象的常量数据在常量缓冲里占用的字节数（144 字节 -> 对齐到 256）。
// 渲染时第 i 个对象的 CBV 地址 = 缓冲基址 + i * kObjectConstantStride。
constexpr UINT kObjectConstantStride = Align256(sizeof(ObjectConstants));

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

bool Renderer::Initialize(ID3D12Device* device, IDXGIFactory4* factory, HWND hwnd, UINT width, UINT height)
{
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
    if (!CreatePipelineState(device)) return false;
    if (!CreateDepthBuffer(device)) return false;
    if (!CreateConstantBuffers(device)) return false;
    if (!CreateAssets(device)) return false;
    if (!CreateRenderItems()) return false;

    // 相机：位于物体上方 + 前方，斜向下俯视原点（约 23° 俯角），透视投影。
    // 比 M5 略微拉远，好让左右两个物体都完整入镜。
    m_camera.SetView({ 0.0f, 2.6f, -6.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
    m_camera.SetPerspective(60.0f, static_cast<float>(m_width) / static_cast<float>(m_height),
                            0.1f, 100.0f);

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
    // 1. 等待当前 frame 上一轮的 GPU 工作完成（可安全复用其 allocator 与 CB）。
    WaitForGpu();

    const UINT backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
    FrameContext& frame = m_frames[m_frameIndex];

    // 2. 计算时间与相机矩阵。
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    const double seconds = static_cast<double>(now.QuadPart - m_startCounter) * m_secondsPerCount;

    const XMMATRIX view = m_camera.GetView();
    const XMMATRIX proj = m_camera.GetProjection();
    const XMMATRIX viewProj = XMMatrixMultiply(view, proj);

    // 光照方向：世界空间中「从表面指向光源」的方向，归一化后交给着色器。
    // 选中左上前方，让球体上亮下暗，立体感清晰。
    XMFLOAT4 lightDirection = {};
    XMStoreFloat4(&lightDirection,
                 XMVector3Normalize(XMVectorSet(-0.45f, 0.75f, -0.48f, 0.0f)));

    // 3. 把每个对象的常量数据写进本帧常量缓冲的对应槽位。
    //
    //    布局：第 i 个对象位于 (uint8*)mapped + i * 256
    //    这里一次性 Map 整块缓冲、写完所有对象再 Unmap —— Map/Unmap 本身有开销，
    //    批量写比「每个对象 Map 一次」更省。
    D3D12_RANGE readRange = { 0, 0 }; // CPU 只写不读
    void* mapped = frame.constantBuffer.Map(0, &readRange);
    if (mapped == nullptr)
    {
        return;
    }

    std::uint8_t* const constantBase = static_cast<std::uint8_t*>(mapped);
    for (std::size_t i = 0; i < m_renderItems.size(); ++i)
    {
        const RenderItem& item = m_renderItems[i];

        // Transform：先缩放，再绕 Y 自转，最后平移到世界位置。
        // row-vector 约定下依次左乘：v * S * R * T。
        const float angle = static_cast<float>(seconds) * item.rotationSpeed;
        const XMMATRIX world =
            XMMatrixScaling(item.scale, item.scale, item.scale) *
            XMMatrixRotationY(angle) *
            XMMatrixTranslation(item.worldPosition.x, item.worldPosition.y, item.worldPosition.z);

        ObjectConstants constants = {};
        // 矩阵约定：DirectXMath 的 XMMATRIX 是 row-major，HLSL 的 float4x4 默认按
        // column-major 读取内存 —— 两者组合后 HLSL 得到的正好是原矩阵的转置，
        // 因此配合 mul(matrix, vector) 即为正确的 row-vector 变换，无需再手动转置。
        XMStoreFloat4x4(&constants.model, world);
        XMStoreFloat4x4(&constants.viewProj, viewProj);
        constants.lightDirection = lightDirection;

        std::memcpy(constantBase + i * kObjectConstantStride, &constants, sizeof(constants));
    }
    frame.constantBuffer.Unmap(0, nullptr);

    // 4. 复用 allocator 与 command list。
    frame.commandAllocator->Reset();
    m_commandList->Reset(frame.commandAllocator.Get(), nullptr);

    ID3D12Resource* backBuffer = m_backBuffers[backBufferIndex].Get();

    // 5. 状态转换 PRESENT -> RENDER_TARGET（back buffer 由显示交给渲染）。
    TransitionBackBuffer(m_commandList.Get(), backBuffer,
                         D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    // 6. 视口 / 裁剪矩形 / 渲染目标绑定。
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

    // 7. 清屏（颜色 + 深度）。
    const float clearColor[4] = { 0.1f, 0.1f, 0.15f, 1.0f };
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // 8. 设置管线状态。
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->SetPipelineState(m_pipelineState.Get());

    // 绑定 shader-visible 的 SRV 描述符堆。
    // 关键顺序：SetGraphicsRootDescriptorTable 传的是「堆内句柄」，
    // 必须先把堆本身设进来，否则这个句柄无从解释。
    ID3D12DescriptorHeap* const descriptorHeaps[] = { m_srvHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, descriptorHeaps);

    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 9. 遍历 RenderItem 列表逐个绘制。
    //    每个对象只需换三样东西：常量缓冲槽位、纹理描述符、顶点/索引缓冲。
    //    这就是「Mesh + Material + Transform」这条可复用渲染路径的全部内容。
    for (std::size_t i = 0; i < m_renderItems.size(); ++i)
    {
        const RenderItem& item = m_renderItems[i];

        // CBV：指向本对象在本帧常量缓冲里的槽位（根参数 0 = b0）。
        const D3D12_GPU_VIRTUAL_ADDRESS constantAddress =
            frame.constantBuffer.GetGPUVirtualAddress() + i * kObjectConstantStride;
        m_commandList->SetGraphicsRootConstantBufferView(0, constantAddress);

        // SRV：从描述符堆中取出该材质对应的那一项（根参数 1 = t0 的描述符表）。
        // 「堆起始 GPU 句柄 + 索引 * 描述符大小」是 D3D12 定位描述符的标准做法 ——
        // 描述符大小由设备决定（CBV_SRV_UAV 常见为 32 字节），必须查询而不能写死。
        D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        srvHandle.ptr += static_cast<UINT64>(item.material->srvIndex) * m_srvDescriptorSize;
        m_commandList->SetGraphicsRootDescriptorTable(1, srvHandle);

        m_commandList->IASetVertexBuffers(0, 1, &item.mesh->GetVertexBufferView());
        m_commandList->IASetIndexBuffer(&item.mesh->GetIndexBufferView());
        m_commandList->DrawIndexedInstanced(item.mesh->GetIndexCount(), 1, 0, 0, 0);
    }

    // 10. 状态转换 RENDER_TARGET -> PRESENT（渲染完交回显示引擎）。
    TransitionBackBuffer(m_commandList.Get(), backBuffer,
                         D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    // 11. 关闭命令列表并提交，Present，Signal。
    m_commandList->Close();
    ID3D12CommandList* const lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    m_swapChain->Present(1, 0);

    ++m_fenceValue;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValue);
    frame.fenceValue = m_fenceValue;

    m_frameIndex = (m_frameIndex + 1) % kFrameCount;
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
    // 每帧一份 Upload Heap 常量缓冲，内部按 256 字节切成 kMaxRenderItems 个槽位，
    // 每个槽位存放一个对象的 ObjectConstants。
    //
    // 为什么每帧一份：CPU 在写第 N 帧的常量时，GPU 可能仍在读第 N-1 / N-2 帧的同一块内存。
    // 「每帧独立 + 帧间 Fence 等待」保证不会写到 GPU 正在读的数据上。
    const UINT cbSize = kObjectConstantStride * kMaxRenderItems;
    for (UINT i = 0; i < kFrameCount; ++i)
    {
        if (!m_frames[i].constantBuffer.Initialize(device, cbSize,
                D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ))
        {
            std::cerr << "[Renderer] Failed to create constant buffer.\n";
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

    if (!LoadAndCreateMesh(device, uploadList.Get(), "assets/sphere.obj",
                           m_sphereMesh, stagingResources))
    {
        return false;
    }
    if (!LoadAndCreateMesh(device, uploadList.Get(), "assets/cube.obj",
                           m_cubeMesh, stagingResources))
    {
        return false;
    }

    // 两张纹理分别占用 SRV 堆的槽位 0 与 1 —— 绘制时靠槽位号切换。
    if (!LoadAndCreateTexture(device, uploadList.Get(), "assets/checker.tga", 0,
                              m_checkerTexture, stagingResources))
    {
        return false;
    }
    if (!LoadAndCreateTexture(device, uploadList.Get(), "assets/uv_grid.tga", 1,
                              m_uvGridTexture, stagingResources))
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
    m_checkerMaterial.srvIndex = m_checkerTexture.GetSrvIndex();
    m_uvGridMaterial.srvIndex = m_uvGridTexture.GetSrvIndex();

    std::cout << "[Renderer] Assets loaded: sphere=" << (m_sphereMesh.GetIndexCount() / 3)
              << " tris, cube=" << (m_cubeMesh.GetIndexCount() / 3)
              << " tris, textures=2, srvDescriptorSize=" << m_srvDescriptorSize << "\n";
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

bool Renderer::CreateRenderItems()
{
    // 场景：左边一个球体（UV 参考纹理），右边一个立方体（棋盘格纹理）。
    // 两者用不同的 Mesh 与不同的 Material，用来证明这条渲染路径是「可复用」的 ——
    // 渲染器只是在遍历列表，并不知道具体画的是什么。
    m_renderItems.clear();

    RenderItem sphere;
    sphere.mesh = &m_sphereMesh;
    sphere.material = &m_uvGridMaterial;
    sphere.worldPosition = { -1.05f, 0.0f, 0.0f };
    sphere.scale = 1.0f;
    sphere.rotationSpeed = 0.6f;
    m_renderItems.push_back(sphere);

    RenderItem cube;
    cube.mesh = &m_cubeMesh;
    cube.material = &m_checkerMaterial;
    cube.worldPosition = { 1.05f, 0.0f, 0.0f };
    cube.scale = 1.0f;
    cube.rotationSpeed = 1.0f;
    m_renderItems.push_back(cube);

    return true;
}

