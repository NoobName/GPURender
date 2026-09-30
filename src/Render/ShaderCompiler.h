#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 用 DXC（dxc.exe）把 HLSL 源文件编译成 DXIL 字节码。
//
// 设计要点：
//   - Shader 源文件保存在 shaders/ 目录（不内嵌成 C++ 字符串），运行时从文件读取。
//   - 编译在程序启动时显式执行（而非构建期），错误信息直接输出到控制台。
//   - 该类不依赖 D3D12 运行时，只负责「把 .hlsl 变成字节码」这一件事。
class ShaderCompiler
{
public:
    // 编译一个 shader。
    //   hlslFileName   : 相对 shaders/ 目录的文件名（如 L"TriangleVS.hlsl"）
    //   entryPoint     : 入口函数名（如 L"main"）
    //   targetProfile  : DXC target（如 L"vs_6_0" / L"ps_6_0"）
    // 返回：编译成功时为 DXIL 字节码；失败时为空，errorMessage 含 dxc 的完整错误输出。
    static std::vector<std::uint8_t> Compile(const std::wstring& hlslFileName,
                                             const std::wstring& entryPoint,
                                             const std::wstring& targetProfile,
                                             std::string& errorMessage);

private:
    // 定位 dxc.exe：优先 DXC_PATH 环境变量，否则用 PATH 里的 dxc。
    static std::wstring FindDxc();
    // shader 目录 = 可执行文件所在目录 + "shaders"
    static std::wstring GetShaderDirectory();
};
