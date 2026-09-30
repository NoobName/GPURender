# GPUDrivenRenderer

一个从零构建的现代 GPU-Driven 实时渲染器，使用 C++20、DirectX 12 与 HLSL。

## 当前状态

- **Milestone M1** ✅：最小 DirectX 12 应用基础 —— Win32 窗口 + D3D12 Device + Adapter 枚举 + Capability 查询。
- **Milestone M2** ✅：Command Submission 和 Presentation —— CommandQueue + SwapChain（三重缓冲）+ RTV Heap + CommandAllocator/List + Fence/Event，持续 Clear + Present。
- **Milestone M3** ✅：渲染第一个 Triangle —— HLSL VS/PS + DXC 编译 + Root Signature + PSO + 顶点缓冲上传 + DrawInstanced（彩色三角形）。
- **Milestone M4** ✅：GPU Resource 和 Upload Infrastructure —— GPUBuffer 抽象 + UploadHelper（Upload Heap staging → CopyBufferRegion → Default Heap）。
- **Milestone M5** ✅：基础 3D Rendering —— Index Buffer + DrawIndexedInstanced + Depth Buffer + DSV + Depth Testing + Camera + MVP 变换 + Constant Buffer（256 对齐），旋转立方体。
- **Milestone M6** ✅：基础 Asset Rendering —— 外部 OBJ 网格 + TGA 纹理加载、Mesh / Material / Transform 可复用渲染路径、SRV Descriptor Heap、静态 Sampler、UV 纹理采样、纹理显存驻留。
- **Milestone M7** ✅：CPU-Driven Large Instance Baseline —— 确定性测试场景（1,000 / 10,000 / 100,000 实例）、InstanceData（world + bounding sphere + mesh/material index）、CPU 循环逐实例提交、CPU 帧时间 / draw call 数 / 可见数统计、屏幕实时统计显示。
- **Milestone M8** ✅：CPU Frustum Culling Baseline —— 视锥六平面提取（Gribb-Hartmann）、World-Space Bounding Sphere、Plane-Sphere 保守测试、只提交可见实例、剔除耗时与 Culled 统计、Culling Toggle、Frustum / Bounding Sphere 调试可视化、相机可转向。
- 尚未实现 GPU-Driven 管线（GPU Resident Scene Data、Compute Shader 剔除、ExecuteIndirect 等）。

## 技术栈

- C++20 / MSVC（Visual Studio 2022）
- DirectX 12 / DXGI
- HLSL / DXC（Shader 编译）
- CMake

## 构建

前置要求：

- Windows 10/11 x64
- Visual Studio 2022（含「使用 C++ 的桌面开发」工作负载）
- CMake 3.24+
- Windows SDK 10.0.19041+（本项目在 10.0.26100 上开发）

### 方式一：构建脚本（推荐）

脚本会自动定位 Visual Studio（`vswhere` → `VSINSTALLDIR` → 常见路径回退）、Windows
SDK 与 CMake，并补齐 Debug 运行库 DLL 的 PATH（`vcvars64.bat` 本身不含该路径）：

```bat
scripts\build_debug.bat    :: 配置（NMake 生成器）+ 编译 Debug x64
scripts\run_debug.bat      :: 运行 Debug
scripts\build_release.bat  :: 编译 Release x64
```

### 方式二：标准 CMake 预设（VS2022 正常安装时）

```powershell
cmake --preset vs2022
cmake --build --preset vs2022-debug
```

运行（Debug 构建会开启 D3D12 Debug Layer，需先安装 Windows 可选功能 **Graphics Tools**）：

```powershell
.\build\vs2022\Debug\GPUDrivenRenderer.exe
```

程序会打开一个窗口，并在控制台输出所选 GPU 与资源加载信息；关闭窗口即可正常退出。

## 运行与操作（M7 Stress Test）

程序默认渲染一个由 1,000 个实例组成的**确定性测试场景**，用于建立 CPU-Driven 渲染基线。

**命令行参数**（便于脚本化采集 benchmark 数据）：

```bat
GPUDrivenRenderer.exe --instances 100000              :: 指定实例数量
GPUDrivenRenderer.exe --instances 100000 --no-cull    :: 提交全部实例（关闭 CPU 剔除）
GPUDrivenRenderer.exe --instances 100000 --yaw 90     :: 指定初始相机朝向（度）
GPUDrivenRenderer.exe --instances 2000 --debug-viz 2  :: 调试可视化：0=关 1=视锥 2=视锥+包围球
GPUDrivenRenderer.exe --help                          :: 查看用法
```

**运行时按键**：

| 按键 | 作用 |
|---|---|
| `1` / `2` / `3` | 切换实例规模为 1,000 / 10,000 / 100,000 |
| `C` | 切换 CPU 视锥剔除（只提交可见实例 ↔ 提交全部） |
| `V` | 循环切换调试可视化（关 → 视锥 → 视锥 + 包围球） |
| `WASD` / 方向键 | 旋转相机（观察 Visible Count 随视角变化） |
| `Space` | 切换自动转头 |
| `R` | 重置到基准视角 |

屏幕左上角实时显示：实例总数、可见数、draw call 数，以及分阶段的 CPU 帧时间
（`update` / `record` / `present`）。控制台每 60 帧输出一行 `[Bench] ...` 便于脚本采集。

> 性能数据与测量方法论记录在本地 `docs/BENCHMARKS.md`（`docs/` 不随仓库分发）。

## 资源与工具

渲染器在运行期从 exe 同级目录加载 `assets/` 下的**外部资源**（构建时由 CMake 拷贝过去）：

| 资源 | 说明 |
|---|---|
| `assets/sphere.obj` | UV 球体，825 顶点 / 1536 三角形（含法线与 UV） |
| `assets/cube.obj` | 带 UV 的立方体 |
| `assets/checker.tga` | 棋盘格纹理（带方向标记，便于验证 UV 方向） |
| `assets/uv_grid.tga` | UV 参考纹理（渐变 + 网格 + 边缘标记） |
| `assets/font_atlas.tga` | 屏幕统计文字用的位图字体图集（128×96，96 个 8×16 字格，最后一格是实心色块） |

这些资源由脚本可复现地生成，不需要手动准备素材：

```powershell
python tools\gen_assets.py                  :: OBJ 网格 + TGA 纹理
powershell -File tools\gen_font_atlas.ps1   :: 字体图集（用 .NET System.Drawing 渲染）
```

> **为什么用 OBJ + TGA 而不是 glTF？**
> 本项目刻意把「文件解析」与「渲染」分成两层，解析器保持极小
> （OBJ ≈150 行、TGA ≈60 行），使精力集中在渲染路径本身。
> `Asset` 层只产出统一的 `MeshData` / `ImageData`，将来若要换成 glTF（`cgltf`），
> 只需替换 loader，渲染代码一行都不用改。

## 文档

项目开发文档（Roadmap / 架构 / 学习笔记 / 基准）保存在本地 `docs/` 目录，仅供开发者
本地维护与学习，不随仓库分发（已加入 `.gitignore`）。
