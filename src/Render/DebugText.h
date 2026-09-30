#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Render/GPUBuffer.h"
#include "Render/Texture.h"

// 极简屏幕空间文本渲染器。
//
// 为什么需要它：M7 要实时显示统计（CPU 帧时间 / draw call 数 / 可见实例数）。
// 常规做法是引入 ImGui，但本项目环境无法获取第三方源码，所以这里自己写了一个
// 只做「画文字」的最小实现 —— 也顺便建立了屏幕空间渲染能力，后续 milestone 的
// debug 可视化（剔除结果、LOD 着色等）可以直接复用。
//
// 实现要点：
//   - 字体是一张位图图集：16 列 x 6 行，每格 8x16 像素，按 ASCII 32..127 排布，
//     由 tools/gen_font_atlas.ps1 生成
//   - 每个字符 = 一个 quad（两个三角形），顶点 = 像素坐标 + 图集 UV
//   - 顶点写进 Upload Heap 动态缓冲，每帧重写一次
//   - 用正交投影把像素坐标映射到 NDC；PSO 关闭深度测试、开启 alpha 混合
class DebugText
{
public:
    static constexpr UINT kCellWidth = 8;
    static constexpr UINT kCellHeight = 16;
    static constexpr UINT kAtlasColumns = 16;
    static constexpr UINT kAtlasRows = 6;
    static constexpr UINT kFirstCharacter = 32;  // ' '
    static constexpr UINT kCharacterCount = kAtlasColumns * kAtlasRows; // 96 -> ' ' .. '~'
    static constexpr UINT kMaxCharacters = 2048; // 单帧最多绘制多少字符

    // fontAtlasPath 由调用方给出（Renderer 负责「exe 目录 + 相对路径」的拼接规则）。
    bool Initialize(ID3D12Device* device,
                    ID3D12GraphicsCommandList* cmd,
                    const std::string& fontAtlasPath,
                    ID3D12DescriptorHeap* srvHeap,
                    UINT srvDescriptorSize,
                    UINT srvIndex,
                    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& stagingOut);

    // 开始一帧：指定屏幕尺寸（像素），并清空上一帧累积的顶点。
    void Begin(UINT screenWidth, UINT screenHeight);

    // 追加一段文本（支持 '\n' 换行）。x / y 为像素坐标，scale 为字符放大倍数。
    void AddText(float x, float y, float scale, const char* text);
    void AddText(float x, float y, float scale, const std::string& text);

    // 画一个纯色矩形（统计文本的半透明背景条）。
    // 实现上复用字体图集最后一格（ASCII 127）—— 生成脚本把它画成了实心色块，
    // 因此把 UV 固定在该格内部再拉伸，就能得到任意尺寸的纯色面片，无需第二套管线。
    void AddRect(float x, float y, float width, float height);

    // 设置后续 AddText 使用的颜色（Begin 时会重置为不透明白色）。
    // 典型用法：先用深色按 (1.5, 1.5) 偏移画一遍当作阴影，再用亮色画一遍本体 ——
    // 这样无论背后是深色天空还是亮色模型，文字都清晰可读。
    void SetColor(float r, float g, float b, float a);

    // 把本帧累积的顶点写进 GPU 缓冲。返回 false 表示写入失败。
    bool End();

    // 记录绘制命令。调用前应已设置好根签名 / PSO / 描述符堆 / 根常量。
    void Render(ID3D12GraphicsCommandList* cmd) const;

    // 屏幕空间正交投影矩阵：左上为原点、y 向下。
    DirectX::XMFLOAT4X4 GetOrthoMatrix() const;

    // 字体图集在 SRV 描述符堆里的槽位号（绘制时需要用它算 GPU 句柄）。
    UINT GetSrvIndex() const { return m_fontTexture.GetSrvIndex(); }

    bool HasContent() const { return !m_vertices.empty(); }

private:
    struct UiVertex
    {
        DirectX::XMFLOAT2 position; // 像素坐标
        DirectX::XMFLOAT2 uv;       // 字体图集 UV
        DirectX::XMFLOAT4 color;    // 文字颜色（含 alpha）
    };
    static_assert(sizeof(UiVertex) == 32, "UiVertex 必须紧凑占 32 字节，与 Input Layout 对应");

    Texture m_fontTexture;
    GPUBuffer m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView = {};
    std::vector<UiVertex> m_vertices;
    DirectX::XMFLOAT4 m_color = { 1.0f, 1.0f, 1.0f, 1.0f };
    UINT m_screenWidth = 0;
    UINT m_screenHeight = 0;
};
