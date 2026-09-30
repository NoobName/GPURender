cmake_minimum_required(VERSION 3.24)

project(GPUDrivenRenderer VERSION 0.1.0 LANGUAGES CXX)

# Milestone 1 uses the console subsystem (not the WIN32 GUI subsystem) so that
# GPU info and Debug Layer logs are visible directly from a command line.
# When we move to a pure rendering loop in a later milestone, we can switch to
# the WIN32 subsystem.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
    message(FATAL_ERROR "GPUDrivenRenderer currently requires MSVC (Visual Studio 2022).")
endif()

add_executable(GPUDrivenRenderer
    src/main.cpp
    src/Application.cpp
    src/Window.cpp
    src/Graphics/D3D12Context.cpp
    src/Render/Renderer.cpp
    src/Render/ShaderCompiler.cpp
    src/Render/GPUBuffer.cpp
    src/Render/UploadHelper.cpp
    src/Render/Camera.cpp
    src/Render/Mesh.cpp
    src/Render/Texture.cpp
    src/Render/DebugText.cpp
    src/Render/DebugLines.cpp
    src/Render/InstanceBuffer.cpp
    src/Render/InstanceValidator.cpp
    src/Render/VisibleInstanceList.cpp
    src/Render/GPUFrustumCuller.cpp
    src/Render/IndirectDrawCommands.cpp
    src/Asset/ObjLoader.cpp
    src/Asset/TgaLoader.cpp
    src/Scene/Scene.cpp
)

target_include_directories(GPUDrivenRenderer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 把 HLSL 源文件与测试资源复制到构建目录：
#   shaders/ -> 运行时由 ShaderCompiler 从 exe 同级目录加载
#   assets/  -> 运行时由 Renderer 从 exe 同级目录加载（OBJ / TGA）
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/shaders DESTINATION ${CMAKE_BINARY_DIR})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/assets DESTINATION ${CMAKE_BINARY_DIR})

target_compile_definitions(GPUDrivenRenderer PRIVATE
    NOMINMAX            # keep windows.h min/max macros from polluting std::min/std::max
    WIN32_LEAN_AND_MEAN # trim windows.h
    UNICODE             # use wide-char Win32 APIs (W variants)
    _UNICODE            # wide-char CRT
)

target_compile_options(GPUDrivenRenderer PRIVATE
    /W4
    /permissive-
    /utf-8              # sources are UTF-8; keeps Chinese comments/strings intact
)

# D3D12 / DXGI runtime libraries (system SDK, no third-party dependency yet).
target_link_libraries(GPUDrivenRenderer PRIVATE
    d3d12
    dxgi
    dxguid
)
