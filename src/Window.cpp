#include "Window.h"

Window::~Window()
{
    Shutdown();
}

bool Window::Initialize(HINSTANCE hInstance, const std::wstring& title, int width, int height)
{
    m_hInstance = hInstance;
    m_title = title;
    m_width = width;
    m_height = height;

    const wchar_t* className = L"GPUDrivenRendererWindow";

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm       = wc.hIcon;
    wc.lpszClassName = className;

    if (!RegisterClassExW(&wc))
    {
        // 同一进程重复初始化时允许"已存在"错误，其余情况视为失败
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }
    }

    // 依据客户区尺寸反推窗口整体尺寸（含标题栏与边框）
    RECT rect = { 0, 0, width, height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(
        0,
        className,
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        hInstance,
        this); // 通过 WM_NCCREATE 的 lpCreateParams 传回 this

    return m_hwnd != nullptr;
}

void Window::Shutdown()
{
    if (m_hwnd != nullptr)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void Window::Show()
{
    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
}

LRESULT CALLBACK Window::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    Window* self = nullptr;

    if (msg == WM_NCCREATE)
    {
        // WM_NCCREATE 是窗口创建后的第一条消息，CREATESTRUCT 里携带 CreateWindowExW 传入的 this
        const auto* createStruct = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<Window*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg)
    {
    case WM_SIZE:
        if (self != nullptr)
        {
            self->m_width  = static_cast<int>(LOWORD(lParam));
            self->m_height = static_cast<int>(HIWORD(lParam));
        }
        return 0;

    case WM_CLOSE:
        // 用户点击关闭按钮：销毁窗口，随后触发 WM_DESTROY
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        // 窗口销毁：投递 WM_QUIT 使消息循环退出（GetMessageW 返回 0）
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
