#pragma once

#include <cstdint>

#include "Window.h"
#include "Graphics/D3D12Context.h"
#include "Render/Renderer.h"

// 组装 Window、D3D12Context 与 Renderer，驱动整个应用的生命周期。
// M2：消息循环改为 PeekMessage game loop，每帧调用 Renderer::Render()。
class Application
{
public:
    // 这些参数都来自命令行，仅用于启动配置；运行中可用按键改变。
    bool Initialize(std::uint32_t initialInstanceCount = 1000,
                    bool useCpuCulling = true,
                    float cameraYawDegrees = 0.0f,
                    int debugViewMode = 0,
                    bool compareCulling = false,
                    bool gpuDriven = false);
    void Run();
    void Shutdown();

private:
    Window m_window;
    D3D12Context m_d3d12;
    Renderer m_renderer;
    bool m_initialized = false;
};
