#pragma once

#include "Window.h"
#include "Graphics/D3D12Context.h"
#include "Render/Renderer.h"

// 组装 Window、D3D12Context 与 Renderer，驱动整个应用的生命周期。
// M2：消息循环改为 PeekMessage game loop，每帧调用 Renderer::Render()。
class Application
{
public:
    bool Initialize();
    void Run();
    void Shutdown();

private:
    Window m_window;
    D3D12Context m_d3d12;
    Renderer m_renderer;
    bool m_initialized = false;
};
