#pragma once

#include <windows.h>
#include <string>

// 纯 Win32 窗口封装。
// 职责：注册窗口类、创建窗口、分发消息、暴露 HWND 与客户区尺寸。
// 本类不包含任何 D3D12 逻辑，保持单一职责。
class Window
{
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool Initialize(HINSTANCE hInstance, const std::wstring& title, int width, int height);
    void Shutdown();
    void Show();

    HWND GetHandle() const { return m_hwnd; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    HINSTANCE m_hInstance = nullptr;
    std::wstring m_title;
    int m_width = 0;
    int m_height = 0;
};
