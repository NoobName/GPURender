@echo off
setlocal

REM ============================================================================
REM  GPUDrivenRenderer - Release x64 build (MSVC via NMake)
REM  Same VS/CMake auto-location as build_debug.bat, but Release configuration.
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
    exit /b 1
)
echo [VS] %VSROOT%

REM ---- 2. Set up MSVC + Windows SDK environment ----
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed
    exit /b 1
)

REM ---- 3. Append Debug CRT DLL dirs (harmless for Release; needed for Debug) ----
set "PATH=%VCToolsRedistDir%debug_nonredist\x64\Microsoft.VC145.DebugCRT;%PATH%"
set "PATH=%WindowsSdkBinPath%%WindowsSDKVersion%x64\ucrt;%PATH%"

REM ---- 4. Locate CMake ----
set "CMAKEEXE="
for /f "delims=" %%i in ('where cmake 2^>nul') do if not defined CMAKEEXE set "CMAKEEXE=%%i"
if not defined CMAKEEXE if exist "C:\Program Files\CMake\bin\cmake.exe" set "CMAKEEXE=C:\Program Files\CMake\bin\cmake.exe"
if not defined CMAKEEXE if exist "%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKEEXE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not defined CMAKEEXE (
    echo [ERROR] CMake not found.
    exit /b 1
)
echo [CMake] %CMAKEEXE%

REM ---- 5. Configure + build ----
set "SRC=%~dp0.."
set "BUILD=%~dp0..\build\nmake-release"

echo [1/2] Configuring (NMake + MSVC, Release x64)...
"%CMAKEEXE%" -S "%SRC%" -B "%BUILD%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
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
