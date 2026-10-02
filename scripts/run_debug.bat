@echo off
setlocal

REM ============================================================================
REM  GPUDrivenRenderer - run Debug x64 build
REM  Same environment setup as build_debug.bat: locate VS -> vcvars -> append
REM  Debug CRT DLL dirs -> run the exe.
REM ============================================================================

REM ---- 1. Locate Visual Studio ----
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

REM ---- 2. Set up MSVC + Windows SDK environment ----
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed
    exit /b 1
)

REM ---- 3. Append Debug CRT DLL dirs ----
set "PATH=%VCToolsRedistDir%debug_nonredist\x64\Microsoft.VC145.DebugCRT;%PATH%"
set "PATH=%WindowsSdkBinPath%%WindowsSDKVersion%x64\ucrt;%PATH%"

REM ---- 4. Run ----
set "RENDERER_EXE=%~dp0..\build\nmake-debug\GPUDrivenRenderer.exe"
if not exist "%RENDERER_EXE%" (
    echo [ERROR] Debug executable not found. Run scripts\build_debug.bat first.
    exit /b 1
)
REM Keep runtime shader files in a writable, project-local directory.
REM setlocal limits these environment changes to this script and its child process.
set "TMP=%~dp0..\build\nmake-debug\temp"
set "TEMP=%TMP%"
if not exist "%TMP%\" mkdir "%TMP%"
if not exist "%TMP%\" (
    echo [ERROR] Cannot create shader temporary directory: "%TMP%"
    exit /b 1
)
"%RENDERER_EXE%" %*
set "RUN_EXIT_CODE=%ERRORLEVEL%"
endlocal & exit /b %RUN_EXIT_CODE%
