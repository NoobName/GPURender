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
- **Milestone M9** ✅：GPU-Resident Scene Instance Data —— 整个场景的实例元数据（变换 / 包围球 / 网格索引 / 材质索引）打包进 DEFAULT Heap 的 `StructuredBuffer`；C++ 与 HLSL 的内存布局契约由 `static_assert` + 运行期三重验证（HLSL `sizeof` 上报、逐实例自洽性检查、逐字节 memcmp）共同保证。
- **Milestone M10** ✅：GPU Frustum Culling —— 第一个基于 Compute Shader 的 Visibility Pass。`[numthreads(64,1,1)]`，一个线程处理一个实例，做 Bounding Sphere vs Frustum Plane 保守测试。视锥平面经 root constants 传入，并配有完整的 UAV Barrier（WAW / RAW）与资源状态转换。
- **Milestone M11** ✅：GPU Stream Compaction —— 用 `InterlockedAdd` 把可见实例压缩成紧凑的 `VisibleInstanceIndices[]`，配 `RWByteAddressBuffer` 计数器。计数器每帧在**同一条命令列表内**清零，正常渲染流程**无任何 CPU 同步**。支持把压缩结果回读校验：越界 ID / 重复 ID / 与 CPU 可见集的差异 / 计数越界，四项全部为 0。
- **Milestone M12** ✅：**ExecuteIndirect —— 第一个真正的 GPU-Driven Rendering Path**。命令生成 CS 写出**一条** `DrawIndexed` 间接命令，其 `InstanceCount` 由 GPU 的可见数决定；顶点着色器用 `SV_InstanceID` 查压缩列表取实例数据。CPU 每帧只提交一次 `ExecuteIndirect`，`MaxCommandCount` 恒为常数 1。
  - **CPU 提交的 draw call：76,744 → 0**
  - **`record`（CPU 记录命令耗时）：1.303 ms → 0.087 ms（15×）**
  - 与 CPU-Driven 逐像素对比：3D 区域 99.998% 相同（9 个像素为深度测试边界差异）
  - 运行中按 `M` 切换两条路径，或用 `--gpu-driven` 启动
- **Milestone M13** ✅：**Depth Prepass Architecture** —— 在 GPU-Driven 的绘制之前插入一遍 depth-only 渲染（无像素着色器，复用同一个 VS 与根签名），主 Pass 以 `LESS_EQUAL` + 不写深度的方式复用它的深度，让 Early-Z 在像素着色之前丢弃被遮挡的片元。
  - 引入 **GPU Timestamp 测量**：把每帧切成 `cull / depth / main` 三段，用环形缓冲延迟 3 帧读回，**不引入任何 CPU 同步**
  - 新增**深度缓冲可视化**（全屏三角形 + 深度线性化，按 `B` 切换）
  - 实测结论（Release，100k 实例）：**本项目场景下 prepass 净亏损** —— ON 1.44 ms vs OFF 0.88 ms。几何量极大（92 万三角形）而 overdraw 极低，主 Pass 的 Early-Z 几乎没省下东西（0.81 vs 0.88 ms），多出来的 0.60 ms 深度 Pass 就是纯亏损。完整分析见 `docs/LEARNING_NOTES.md` M13 Q3
  - ⚠️ 本阶段的测量曾一度被**显存压力**污染（读数虚高 10 倍以上，并导致一个关于 `NumRenderTargets` 的错误结论）。释放显存后重测并修正了全部文档 —— 这个教训本身也记入了学习笔记 1.7 节
- **Milestone M14** ✅：**Hierarchical Z-Buffer Generation** —— 用 Depth Prepass 的深度在 GPU 上逐级 **Max 归约**，构建从 640x360 到 **1x1** 的完整深度金字塔（10 级，全部由 Compute Shader 生成，**零 CPU 回读**）。
  - **Reduction Convention 已显式记录**：D3D 传统深度（近 0 远 1，越大越远）→ **Max**。保守性证明：z_obj > max(D) 意味着物体比该区域内所有像素都远，判定「被遮挡」是**充分**的；换成 Min 会误剔除可见物体
  - 每级之间用 **UAV barrier** 同步（整条链常驻 UNORDERED_ACCESS，状态转换帮不上忙）
  - 每个 mip 一个 **UAV 描述符并连续排列**，使降采样能用一张覆盖 2 个描述符的表同时绑定「源 mip / 目标 mip」
  - **任意 mip 级均可可视化**（`N` 键 / `--hzb-viz`，`[` `]` 或 `--hzb-mip N` 选级）
  - **实测验证**（帧 60 一次性读回）：10 级中心深度**单调不减**（HZB[i] <= HZB[i+1]，Max 归约的必要条件），全部落在 [0,1]，最后一级 1x1 = **1.000000** 正是背景深度 —— 三点校验全部 PASSED
- **Milestone M15** ✅：**HZB GPU Occlusion Culling** —— 用 M14 的深度金字塔对视锥内候选做**保守**遮挡测试，把确定看不见的实例从 Main Pass 的绘制列表里去掉。
  - 六步判定：包围球投影 → 屏幕矩形估计（半径分母是 sqrt(d²−r²)）→ 按投影尺寸选 mip → 采样保守深度（区域 max）→ 与物体最近深度比较 → Visible/Occluded
  - **六处「偏向 Visible」的保守设计**：球在相机后 / 穿近平面 / 完全出屏 / mip 用 ceil 取更粗 / 采样外扩 1 像素 / 深度比较加 bias。**屏幕边缘只要与画面有重叠就一定走完整测试**，不会随机消失
  - **Visualization Mode**：`K` / `--occlusion-viz` 把包围球按三类状态着色 —— 绿 = 最终可见、红 = 视锥剔除、**黄 = 通过视锥但被遮挡剔除**（实现上零新增 GPU 资源，用 CPU 视锥内集合与 GPU 可见列表作差）
  - **Frustum Culling 与 Occlusion Culling 分开统计**（候选数 / 遮挡剔除数 / 保守放行数 / HZB 采样数），统计走延迟读回，**不引入同步**
  - **实测正确性**：occlusion ON vs OFF 画面逐像素对比 **0 / 432,000 差异（0.000%）** —— False Positive Culling = **0**
  - **实测剔除率**：10k 实例下 7,684 个候选中剔除 **6,810（88.6%）**，仅 61 个走保守放行
  - ⚠️ **实测性能：本场景下净亏损**（gpuMain +27%~+51%）。原因：M13 的 Depth Prepass + Early-Z **已经**让被遮挡片元在像素着色前被丢弃，所以遮挡剔除只剩「省顶点处理」这点收益，抵不过 occlusion CS 的随机 HZB 采样。完整分析与「什么场景它才会赢」见 docs/LEARNING_NOTES.md M15 Q4
  - 已知 Limitation 共 9 条（two-phase、AABB 支持、采样次数偏多、可视化读回会打断流水线等）已记录在学习笔记第 4 节
- **Milestone M16** ✅：**GPU-Driven LOD Selection** —— 每个 Mesh 提供 4 级 Index Range（细分立方体 768/192/48/12 三角形），由**剔除 CS 按投影后的屏幕尺寸**选级，间接命令直接使用该级的几何偏移。CPU 每帧都不再为任何实例选 LOD。
  - **每个 LOD 一条间接命令**：几何选择只能表达在命令的 IndexCount / StartIndexLocation / BaseVertexLocation 里，所以 M12 的「全世界一条命令」必须演进成「每级一条」——ExecuteIndirect 次数从常数 1 变成常数 N（=4），但每条命令的 InstanceCount 仍完全由 GPU 决定
  - **判据**：screenSize = 2·r·P11 / sqrt(d²−r²)（球到切平面的距离）→ **占屏幕高度比例**，阈值与分辨率无关
  - **每段一个 SRV**，让顶点着色器**完全不需要知道 LOD 存在**（把「着色器需要新参数」转化成「绑定需要换一张表」）
  - **实测**：LOD 分布 469/4332/2862/21，**渲染三角形减少 77.5%**（1,329,564 vs 5,901,312）
  - **命令缓冲与实例数彻底解耦**：从 M12 的 maxInstances × 20 B（2 MB）变成 lodCount × 20 B（**80 B**）
  - 可视化：`L` 键 / `--debug-viz 2`，包围球按 GPU 实际选中的 LOD 着色（**红=LOD0 / 黄=LOD1 / 绿=LOD2 / 蓝=LOD3**）
  - 已知 Limitation 8 条（与 M15 遮挡剔除互斥、无网格简化、无 hysteresis 与 morphing 等）见学习笔记 M16 第 3 节
- 尚未实现 two-phase occlusion culling、per-LOD 遮挡剔除与 Meshlet。

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

验证 M16 的 GPU LOD（在项目根目录的 CMD 中运行）：

```bat
scripts\build_debug.bat
scripts\run_debug.bat --instances 10000 --gpu-driven --no-occlusion
```

`run_debug.bat` 会准备 Debug 运行库环境，并把全部命令行参数转交给程序。
当前项目不提供 `build\nmake-debug\run_m8.bat`，请使用上面的正式启动脚本。
M16 的 LOD 与遮挡剔除暂时互斥，因此此处使用 `--no-occlusion`。
需要输出 GPU/CPU 可见集和 LOD 分布校验时，再加上 `--compare-cull`。

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

运行时 DXC 编译需要写入临时 DXIL 和日志文件。程序优先使用 Windows 临时目录
（由 `TMP` / `TEMP` 等环境变量决定）；若目录不存在或无法写入，会自动回退到
exe 同级的 `shader-temp/` 目录，并在每次编译结束后清理本次临时文件。
如果两个位置都不可用，错误日志会分别列出路径和 Windows 错误码。
`Win32 error 5` 表示访问被拒绝，此时应检查目录写入权限或安全软件的拦截记录。

## 运行与操作（M7 Stress Test）

程序默认渲染一个由 1,000 个实例组成的**确定性测试场景**，用于建立 CPU-Driven 渲染基线。

**命令行参数**（便于脚本化采集 benchmark 数据）：

```bat
GPUDrivenRenderer.exe --instances 100000              :: 指定实例数量
GPUDrivenRenderer.exe --instances 100000 --no-cull    :: 提交全部实例（关闭 CPU 剔除）
GPUDrivenRenderer.exe --instances 100000 --yaw 90     :: 指定初始相机朝向（度）
GPUDrivenRenderer.exe --instances 2000 --debug-viz 2  :: 调试可视化：0=关 1=视锥 2=视锥+包围球
GPUDrivenRenderer.exe --compare-cull                  :: 启动时做一次 GPU vs CPU 剔除结果对比
GPUDrivenRenderer.exe --gpu-driven                    :: 用 ExecuteIndirect 渲染（GPU-Driven）
GPUDrivenRenderer.exe --no-prepass                    :: 关闭 Depth Prepass（单遍渲染）
GPUDrivenRenderer.exe --depth-viz                     :: 显示深度缓冲
GPUDrivenRenderer.exe --hzb-viz                       :: 显示 HZB 深度金字塔
GPUDrivenRenderer.exe --hzb-mip 5                     :: 显示 HZB 第 5 级 mip
GPUDrivenRenderer.exe --no-occlusion                  :: 关闭 HZB 遮挡剔除（A/B 对比）
GPUDrivenRenderer.exe --occlusion-viz                 :: 可视化被遮挡剔除的实例
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
| `T` | 在 GPU 上重新验证实例数据（C++ / HLSL 布局一致性） |
| `G` | 对比 GPU 与 CPU 的可见集（回读压缩列表，校验越界 / 重复 / 集合差异） |
| `M` | 切换渲染路径：CPU-Driven（逐个实例提交）↔ GPU-Driven（一次 ExecuteIndirect） |
| `P` | 切换 Depth Prepass（切换时会重建 PSO） |
| `B` | 切换深度缓冲可视化 |
| `N` | 切换 HZB 金字塔可视化 |
| `[` / `]` | 选择要查看的 HZB mip 级（更细 / 更粗） |
| `H` | 开关 HZB 构建 |
| `O` | 开关 HZB 遮挡剔除 |
| `K` | 可视化被遮挡剔除的实例 |

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
