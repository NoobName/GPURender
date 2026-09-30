#include "Render/ShaderCompiler.h"

#include <windows.h>

#include <fstream>
#include <sstream>

namespace
{

std::string ReadFileToString(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return {};
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::vector<std::uint8_t> ReadFileToBytes(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        return {};
    }
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<size_t>(size));
    if (size > 0)
    {
        file.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    return bytes;
}

// 宽字符串转 UTF-8 窄字符串（避免 wchar_t -> char 的隐式截断）
std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty())
    {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::string WindowsError(const std::string& action, const std::wstring& path, DWORD code)
{
    wchar_t message[1024] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, message, 1024, nullptr);
    return action + "\nPath: " + WideToUtf8(path) + "\nWin32 error " +
           std::to_string(code) + ": " + WideToUtf8(message);
}

// 每次编译独占临时文件；退出任意分支时只清理本次创建的文件。
struct TemporaryFiles
{
    std::wstring output;
    std::wstring log;

    ~TemporaryFiles()
    {
        if (!output.empty()) DeleteFileW(output.c_str());
        if (!log.empty()) DeleteFileW(log.c_str());
    }
};

} // namespace

std::vector<std::uint8_t> ShaderCompiler::Compile(const std::wstring& hlslFileName,
                                                  const std::wstring& entryPoint,
                                                  const std::wstring& targetProfile,
                                                  std::string& errorMessage)
{
    errorMessage.clear();

    const std::wstring dxc = FindDxc();
    const std::wstring hlslPath = GetShaderDirectory() + hlslFileName;

    // 检查源文件存在，给更友好的错误
    if (GetFileAttributesW(hlslPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::wostringstream ws;
        ws << L"Shader source not found: " << hlslPath;
        errorMessage = WideToUtf8(ws.str());
        return {};
    }

    // 输出到临时目录（DXIL 字节码 + 错误日志），不污染源码目录
    wchar_t tempDir[MAX_PATH] = {};
    const DWORD tempLength = GetTempPathW(MAX_PATH, tempDir);
    if (tempLength == 0 || tempLength >= MAX_PATH)
    {
        const DWORD code = tempLength == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        errorMessage = WindowsError("Failed to resolve shader temporary directory.", tempDir, code);
        return {};
    }

    TemporaryFiles temporary;
    wchar_t tempFile[MAX_PATH] = {};
    if (GetTempFileNameW(tempDir, L"gpr", 0, tempFile) == 0)
    {
        errorMessage = WindowsError("Failed to create temporary DXIL file.", tempDir, GetLastError());
        return {};
    }
    temporary.output = tempFile;
    if (GetTempFileNameW(tempDir, L"gpr", 0, tempFile) == 0)
    {
        errorMessage = WindowsError("Failed to create temporary DXC log file.", tempDir, GetLastError());
        return {};
    }
    temporary.log = tempFile;
    const std::wstring& outputPath = temporary.output;
    const std::wstring& errorPath = temporary.log;

    // 命令行：dxc -T <profile> -E <entry> <input> -Fo <output>
    // -Fo 指定编译产物输出文件；错误信息由 dxc 写到 stderr。
    const std::wstring cmdline = L"\"" + dxc + L"\" -T " + targetProfile + L" -E " + entryPoint +
                                 L" \"" + hlslPath + L"\" -Fo \"" + outputPath + L"\"";

    // stderr（+ stdout）重定向到文件，避免用管道（且能完整保留 dxc 的多行错误）。
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE logFile = CreateFileW(errorPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logFile == INVALID_HANDLE_VALUE)
    {
        errorMessage = WindowsError("Failed to open temporary DXC log file.", errorPath, GetLastError());
        return {};
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = logFile;
    si.hStdError = logFile;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};
    std::wstring mutableCmdline = cmdline;
    const BOOL launched = CreateProcessW(nullptr, mutableCmdline.data(), nullptr, nullptr,
                                         TRUE, 0, nullptr, nullptr, &si, &pi);
    if (!launched)
    {
        const DWORD code = GetLastError();
        CloseHandle(logFile);
        errorMessage = WindowsError("Failed to launch dxc.exe. Check DXC_PATH or the Windows SDK.", dxc, code);
        return {};
    }

    const DWORD waitResult = WaitForSingleObject(pi.hProcess, INFINITE);
    const DWORD waitError = waitResult == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    DWORD exitCode = 0;
    const BOOL gotExitCode = GetExitCodeProcess(pi.hProcess, &exitCode);
    const DWORD exitError = gotExitCode ? ERROR_SUCCESS : GetLastError();
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(logFile);

    if (waitResult != WAIT_OBJECT_0 || !gotExitCode)
    {
        errorMessage = WindowsError("Failed to wait for DXC completion.", dxc,
                                    waitResult != WAIT_OBJECT_0 ? waitError : exitError);
        return {};
    }

    if (exitCode != 0)
    {
        // 编译失败：把 dxc 的 stderr 全文作为错误信息返回（含文件/行号）
        errorMessage = ReadFileToString(errorPath);
        if (errorMessage.empty())
        {
            errorMessage = "dxc failed with unknown error.";
        }
        return {};
    }

    std::vector<std::uint8_t> bytecode = ReadFileToBytes(outputPath);
    if (bytecode.empty())
    {
        errorMessage = "dxc succeeded but produced no bytecode.";
    }
    return bytecode;
}

std::wstring ShaderCompiler::FindDxc()
{
    // 1. DXC_PATH 环境变量（显式指定完整路径，最高优先级）
    wchar_t buffer[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"DXC_PATH", buffer, MAX_PATH) > 0)
    {
        const std::wstring path(buffer);
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            return path;
        }
    }

    // 2. Windows SDK 自带的 dxc（枚举 bin 目录找最新版本，不依赖 PATH）
    WIN32_FIND_DATAW findData = {};
    const std::wstring pattern = L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\*";
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &findData);
    std::wstring latestVersion;
    while (hFind != INVALID_HANDLE_VALUE)
    {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            findData.cFileName[0] != L'.')
        {
            const std::wstring candidate =
                L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\" +
                std::wstring(findData.cFileName) + L"\\x64\\dxc.exe";
            if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES &&
                std::wstring(findData.cFileName) > latestVersion)
            {
                latestVersion = findData.cFileName;
            }
        }
        if (!FindNextFileW(hFind, &findData))
        {
            break;
        }
    }
    if (hFind != INVALID_HANDLE_VALUE)
    {
        FindClose(hFind);
    }
    if (!latestVersion.empty())
    {
        return L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\" + latestVersion + L"\\x64\\dxc.exe";
    }

    // 3. PATH 里的 dxc（最后兜底）
    return L"dxc.exe";
}

std::wstring ShaderCompiler::GetShaderDirectory()
{
    // shader 目录 = exe 所在目录 + "shaders"
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    const size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
    {
        dir = dir.substr(0, pos);
    }
    return dir + L"\\shaders\\";
}
