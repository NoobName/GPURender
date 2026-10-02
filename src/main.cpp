#include "Application.h"

#include <windows.h>

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
    // 启动后自动做一次 GPU vs CPU 剔除对比（等价于按一次 G 键），
    // 让这个验收项可以被脚本化验证，而不必手动按键。
    bool compareCulling = false;
    // M12：渲染路径。默认 CPU-Driven，保持与 M8~M11 的 benchmark 可比；
    // --gpu-driven 切到 ExecuteIndirect 路径。
    bool gpuDriven = false;
    // M13：Depth Prepass 与深度可视化（默认 prepass 开、可视化关）
    bool depthPrepass = true;
    bool depthVisualize = false;
    // M14：HZB
    bool hzbVisualize = false;
    int hzbMip = 0;
    // M15：HZB 遮挡剔除（默认开；--no-occlusion 关闭以做 A/B 对比）
    bool occlusion = true;
    bool occlusionViz = false;
    // M18：以 Mesh Shader 模式启动。
    // 只表达「意图」—— 硬件不支持时由 Renderer 静默退回默认模式。
    bool meshShader = false;
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
        else if (std::strcmp(arg, "--compare-cull") == 0)
        {
            config.compareCulling = true;
        }
        else if (std::strcmp(arg, "--gpu-driven") == 0)
        {
            config.gpuDriven = true;
        }
        else if (std::strcmp(arg, "--no-prepass") == 0)
        {
            config.depthPrepass = false;
        }
        else if (std::strcmp(arg, "--depth-viz") == 0)
        {
            config.depthVisualize = true;
        }
        else if (std::strcmp(arg, "--hzb-viz") == 0)
        {
            config.hzbVisualize = true;
        }
        else if (std::strcmp(arg, "--no-occlusion") == 0)
        {
            config.occlusion = false;
        }
        else if (std::strcmp(arg, "--mesh-shader") == 0)
        {
            config.meshShader = true;
        }
        else if (std::strcmp(arg, "--occlusion-viz") == 0)
        {
            config.occlusionViz = true;
        }
        else if (std::strcmp(arg, "--hzb-mip") == 0 && i + 1 < argc)
        {
            const long value = std::strtol(argv[++i], nullptr, 10);
            if (value >= 0)
            {
                config.hzbMip = static_cast<int>(value);
                config.hzbVisualize = true; // 指定 mip 就意味着要看它
            }
            else
            {
                std::cerr << "[main] invalid --hzb-mip value\n";
            }
        }
        else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0)
        {
            std::cout << "usage: GPUDrivenRenderer [--instances N] [--cull | --no-cull]\n"
                         "  --instances N  instance count for the stress scene (default 1000)\n"
                         "  --cull         submit only instances that pass the CPU frustum test (default)\n"
                         "  --no-cull      submit every instance (pure CPU-driven baseline)\n"
                         "  --yaw D        initial camera yaw in degrees (default 0)\n"
                         "  --debug-viz N  0 = off, 1 = frustum, 2 = frustum + bounding spheres\n"
                         "  --compare-cull run one GPU-vs-CPU frustum culling comparison at startup\n"
                         "  --gpu-driven   use ExecuteIndirect (GPU-driven) instead of per-instance draws\n"
                         "  --no-prepass   disable the depth prepass (single-pass rendering)\n"
                         "  --depth-viz    visualize the depth buffer\n"
                         "  --hzb-viz      visualize the HZB depth pyramid\n"
                         "  --hzb-mip N    visualize HZB mip level N (implies --hzb-viz)\n"
                         "  --no-occlusion disable HZB occlusion culling\n"
                         "  --occlusion-viz visualize occlusion-culled instances\n"
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
    // 日志和 Win32 错误文本均为 UTF-8，匹配控制台编码，避免中文乱码。
    SetConsoleOutputCP(CP_UTF8);

    // 让 stdout 不带缓冲：崩溃时缓冲区内容会丢失，
    // 逐行落盘才能看到「崩溃前最后到达了哪一步」。
    // 这个习惯在排查 M11 的一次设备移除时直接定位到了崩溃点。
    std::cout << std::unitbuf;
    const AppConfig config = ParseCommandLine(argc, argv);
    if (!config.valid)
    {
        return EXIT_SUCCESS;
    }

    // Application 持有整个 Renderer（各帧资源、HZB、描述符等），
    // 用堆分配而不是栈对象 —— 这是个好习惯，也让将来继续加成员时
    // 不必担心主线程默认 1 MB 的栈预算。
    auto app = std::make_unique<Application>();

    if (!app->Initialize(config.instanceCount, config.useCpuCulling,
                         config.cameraYawDegrees, config.debugViewMode,
                         config.compareCulling, config.gpuDriven,
                         config.depthPrepass, config.depthVisualize,
                         config.hzbVisualize, static_cast<std::uint32_t>(config.hzbMip),
                         config.occlusion, config.occlusionViz,
        config.meshShader))
    {
        return EXIT_FAILURE;
    }

    app->Run();
    app->Shutdown();
    return EXIT_SUCCESS;
}
