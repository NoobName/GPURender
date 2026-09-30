#include "Application.h"

#include <cstdlib>

// 程序入口。
// M1 职责：创建 Application -> 初始化（窗口 + D3D12 设备）-> 跑消息循环 -> 正确关闭。
int main()
{
    Application app;

    if (!app.Initialize())
    {
        return EXIT_FAILURE;
    }

    app.Run();
    app.Shutdown();
    return EXIT_SUCCESS;
}
