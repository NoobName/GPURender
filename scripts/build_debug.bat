@echo off
setlocal

REM ============================================================================
REM  GPUDrivenRenderer - Debug x64 build (MSVC via NMake)
REM
REM  Locates Visual Studio automatically:
REM    1. vswhere (standard VS installs, registered with the system)
REM    2. VSINSTALLDIR env var (manual override)
REM    3. D:\VisualStudio fallback (non-standard install on this machine)
REM
REM  After calling vcvars64.bat, we append the Debug CRT DLL dirs to PATH using
REM  its env vars (VCToolsRedistDir / WindowsSdkBinPath / WindowsSDKVersion),
REM  because vcvars64.bat itself does NOT add them and a /MDd Debug exe would
REM  otherwise hang at startup (missing vcruntime140d.dll / ucrtbased.dll).
REM
REM  NMake generator is used instead of Ninja: in this sandbox Ninja's
REM  static-library archive rule (a cmd.exe /C wrapper) hangs when capturing
REM  child-process output. On a normal machine either generator works.
REM ============================================================================

REM ---- 1. Locate Visual Studio (with C++ x64 tools) ----
set "VSROOT="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
)
if not defined VSROOT if defined VSINSTALLDIR set "VSROOT=%VSINSTALLDIR%"
if not defined VSROOT if exist "D:\VisualStudio\VC\Auxiliary\Build\vcvars64.bat" set "VSROOT=D:\VisualStudio"

if not defined VSROOT (
    echo [ERROR] Visual Studio with C++ tools not found.
    echo         Install VS2022 with the "Desktop development with C++" workload,
    echo         or set VSINSTALLDIR to your VS installation path.
    exit /b 1
)
echo [VS] %VSROOT%

REM ---- 2. Set up MSVC + Windows SDK environment ----
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed
    exit /b 1
)

REM ---- 3. Append Debug CRT DLL dirs (not added by vcvars64.bat) ----
set "PATH=%VCToolsRedistDir%debug_nonredist\x64\Microsoft.VC145.DebugCRT;%PATH%"
set "PATH=%WindowsSdkBinPath%%WindowsSDKVersion%x64\ucrt;%PATH%"

REM ---- 4. Locate CMake ----
set "CMAKEEXE="
for /f "delims=" %%i in ('where cmake 2^>nul') do if not defined CMAKEEXE set "CMAKEEXE=%%i"
if not defined CMAKEEXE if exist "C:\Program Files\CMake\bin\cmake.exe" set "CMAKEEXE=C:\Program Files\CMake\bin\cmake.exe"
if not defined CMAKEEXE if exist "%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKEEXE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not defined CMAKEEXE (
    echo [ERROR] CMake not found. Install CMake or add it to PATH.
    exit /b 1
)
echo [CMake] %CMAKEEXE%

REM ---- 5. Configure + build ----
set "SRC=%~dp0.."
set "BUILD=%~dp0..\build\nmake-debug"

echo [1/2] Configuring (NMake + MSVC, Debug x64)...
"%CMAKEEXE%" -S "%SRC%" -B "%BUILD%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug
if errorlevel 1 (
    echo [ERROR] CMake configure failed
    exit /b 1
)

echo [2/2] Building...
"%CMAKEEXE%" --build "%BUILD%"
if errorlevel 1 (
    echo [ERROR] Build failed
    exit /b 1
)

echo.
echo Build succeeded: %BUILD%\GPUDrivenRenderer.exe
endlocal
