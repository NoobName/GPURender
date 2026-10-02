#include "Application.h"

#include <windows.h>

bool Application::Initialize(std::uint32_t initialInstanceCount, bool useCpuCulling,
                             float cameraYawDegrees, int debugViewMode, bool compareCulling,
                             bool gpuDriven, bool depthPrepass, bool depthVisualize,
                             bool hzbVisualize, std::uint32_t hzbMip,
                             bool occlusion, bool occlusionViz, bool meshShader)
{
    // 1. D3D12 上下文（Debug Layer -> Factory -> Adapter -> Device）
    if (!m_d3d12.Initialize())
    {
        return false;
    }

    // 2. Win32 窗口（先于 Renderer 创建，因为 SwapChain 需要 HWND）
    const HINSTANCE hInstance = GetModuleHandleW(nullptr);
    if (!m_window.Initialize(hInstance, L"GPUDrivenRenderer - M7: CPU-Driven Instance Baseline",
                             1280, 720))
    {
        return false;
    }
    m_window.Show();

    // 3. 渲染器（需要 device + factory + hwnd）
    if (!m_renderer.Initialize(m_d3d12.GetDevice(), m_d3d12.GetFactory(),
                               m_window.GetHandle(), m_window.GetWidth(), m_window.GetHeight(),
                               initialInstanceCount, useCpuCulling,
                               cameraYawDegrees, debugViewMode, compareCulling, gpuDriven,
                               depthPrepass, depthVisualize, hzbVisualize, hzbMip,
                               occlusion, occlusionViz,
                               // M18：Feature Fallback 的入口。
                               // 只有查询到 Tier 1 才允许进入 Mesh Shader 模式；
                               // 否则 M 键会跳过它，不会尝试创建 PSO 或 DispatchMesh。
                               m_d3d12.GetCapabilities().meshShaderTier >= D3D12_MESH_SHADER_TIER_1,
                               meshShader))
    {
        return false;
    }

    m_initialized = true;
    return true;
}

void Application::Run()
{
    // Game loop：PeekMessage 非阻塞，每帧先处理窗口消息、再渲染。
    // WM_QUIT 由 Window::WindowProc 在 WM_DESTROY 时投递，收到即退出循环。
    MSG msg = {};
    while (true)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                return;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // 把窗口收集到的按键转给渲染器（切换实例规模 / 剔除开关）。
        // Window 本身不认识 Renderer，Application 负责把两者接起来。
        for (const UINT key : m_window.TakePendingKeyPresses())
        {
            m_renderer.HandleKey(key);
        }

        m_renderer.Render();
    }
}

void Application::Shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    // 释放顺序：先 Renderer（内部会等待 GPU 完成所有 in-flight 工作），
    // 再销毁窗口，最后释放 D3D12 上下文（导出 Debug 消息并检漏）。
    m_renderer.Shutdown();
    m_window.Shutdown();
    m_d3d12.Shutdown();

    m_initialized = false;
}
