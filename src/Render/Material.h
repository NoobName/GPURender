#pragma once

#include <d3d12.h>

#include <DirectXMath.h>

// 材质：M6 只需要「一张基础颜色纹理 + tint」。
//
// srvIndex 是这张纹理在 shader-visible SRV 描述符堆中的槽位号。
// 绘制时用它算出 SetGraphicsRootDescriptorTable 需要的 GPU 句柄：
//
//     gpuHandle.ptr = heapStart.ptr + srvIndex * descriptorSize
//
// 这正是「描述符堆 + 索引」绑定方式的核心：材质并不直接持有纹理，
// 它只持有「去哪儿找这张纹理」的位置信息。
struct Material
{
    UINT srvIndex = 0;
    DirectX::XMFLOAT4 tint = { 1.0f, 1.0f, 1.0f, 1.0f };
};
