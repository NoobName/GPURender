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
    src/Render/GPUProfiler.cpp
    src/Render/HierarchicalZBuffer.cpp
    src/Render/HZBOcclusionCuller.cpp
    src/Render/MeshLOD.cpp
    src/Render/MeshletBuilder.cpp
    src/Render/MeshletResources.cpp
    src/Asset/ObjLoader.cpp
    src/Asset/TgaLoader.cpp
    src/Scene/Scene.cpp

    # ---- Microsoft DirectXMesh（M17）-----------------------------------------
    # 只编译 meshlet 预处理实际需要的两个翻译单元，而不是把整个库拉进来：
    #   DirectXMeshletGenerator.cpp -> ComputeMeshlets / ComputeCullData
    #   DirectXMeshAdjacency.cpp    -> GenerateAdjacencyAndPointReps
    #                                   （ComputeMeshlets 在未传 adjacency 时内部调用它）
    # 其余 .cpp（Normals / Optimize* / Remap / VBReader / Validate ...）本项目用不到，
    # 刻意不加入，避免无谓的编译时间与代码体积。
    third_party/DirectXMesh/DirectXMeshletGenerator.cpp
    third_party/DirectXMesh/DirectXMeshAdjacency.cpp
)

target_include_directories(GPUDrivenRenderer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # DirectXMesh 的公开头（DirectXMesh.h / .inl）与内部头（DirectXMeshP.h / scoped.h）
    # 都在同一目录，按原样 include，不做任何路径改写。
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/DirectXMesh
)

# 第三方库不参与本项目的 /W4 洁癖：DirectXMesh 的 P.h 自带一批 pragma warning(disable)，
# 但从 /W4 降到 /W3 可以避免它的旧式代码在 /permissive- 下产生噪音，
# 从而让「本项目代码零警告」这个约束仍然有意义。
set_source_files_properties(
    third_party/DirectXMesh/DirectXMeshletGenerator.cpp
    third_party/DirectXMesh/DirectXMeshAdjacency.cpp
    PROPERTIES COMPILE_OPTIONS "/W3;/permissive"
)

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
