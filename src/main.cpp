#include "Application.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
// 应用启动配置（来自命令行）。
//
// 为什么把这些做成命令行参数：M7 的 benchmark 要在 1k / 10k / 100k 三档规模、
// 以及「提交全部」和「只提交可见」两种策略下各跑一遍并记录数据。
// 运行时按键适合人工观察，但脚本化采集需要能在启动时就确定这些开关。
struct AppConfig
{
    std::uint32_t instanceCount = 1000;
    // M8 的主题就是「只提交可见实例」，所以默认开启；
    // --no-cull 可以关掉它，用来对比两种策略下的 draw 数量与 CPU 提交成本。
    bool useCpuCulling = true;
    // 相机水平角：用来脚本化地验证「转到不同视角时 Visible Count 会变化」。
    float cameraYawDegrees = 0.0f;
    int debugViewMode = 0; // 0 = 关闭可视化，1 = 视锥，2 = 视锥 + 包围球
    bool valid = true;
};

AppConfig ParseCommandLine(int argc, char** argv)
{
    AppConfig config;

    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];

        if (std::strcmp(arg, "--instances") == 0 && i + 1 < argc)
        {
            const long value = std::strtol(argv[++i], nullptr, 10);
            if (value > 0)
            {
                config.instanceCount = static_cast<std::uint32_t>(value);
            }
            else
            {
                std::cerr << "[main] invalid --instances value, using default\n";
            }
        }
        else if (std::strcmp(arg, "--cull") == 0)
        {
            config.useCpuCulling = true; // 默认已是 true，这里为了脚本可读性显式保留
        }
        else if (std::strcmp(arg, "--no-cull") == 0)
        {
            config.useCpuCulling = false;
        }
        else if (std::strcmp(arg, "--yaw") == 0 && i + 1 < argc)
        {
            config.cameraYawDegrees = static_cast<float>(std::atof(argv[++i]));
        }
        else if (std::strcmp(arg, "--debug-viz") == 0 && i + 1 < argc)
        {
            const long value = std::strtol(argv[++i], nullptr, 10);
            config.debugViewMode = (value < 0) ? 0 : ((value > 2) ? 2 : static_cast<int>(value));
        }
        else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0)
        {
            std::cout << "usage: GPUDrivenRenderer [--instances N] [--cull | --no-cull]\n"
                         "  --instances N  instance count for the stress scene (default 1000)\n"
                         "  --cull         submit only instances that pass the CPU frustum test (default)\n"
                         "  --no-cull      submit every instance (pure CPU-driven baseline)\n"
                         "  --yaw D        initial camera yaw in degrees (default 0)\n"
                         "  --debug-viz N  0 = off, 1 = frustum, 2 = frustum + bounding spheres\n"
                         "  (runtime keys: 1/2/3 = count, C = toggle culling, V = debug viz,\n"
                         "                 WASD/Arrows = rotate camera, Space = auto orbit, R = reset)\n";
            config.valid = false;
        }
        else
        {
            std::cerr << "[main] unknown argument: " << arg << "\n";
        }
    }

    return config;
}
} // namespace

// 程序入口。
int main(int argc, char** argv)
{
    const AppConfig config = ParseCommandLine(argc, argv);
    if (!config.valid)
    {
        return EXIT_SUCCESS;
    }

    Application app;

    if (!app.Initialize(config.instanceCount, config.useCpuCulling,
                        config.cameraYawDegrees, config.debugViewMode))
    {
        return EXIT_FAILURE;
    }

    app.Run();
    app.Shutdown();
    return EXIT_SUCCESS;
}
