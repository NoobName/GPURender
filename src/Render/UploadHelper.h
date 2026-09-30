#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

#include "Render/GPUBuffer.h"

using Microsoft::WRL::ComPtr;

// 简单的上传辅助：把 CPU 数据经 Upload Heap 拷贝到 Default Heap。
//
// 职责边界刻意收窄：只负责「创建 staging + 写数据 + 记录 copy 命令 + 记录状态转换」。
// 不负责执行命令列表、不负责 fence 等待——这些由调用者控制，保证同步逻辑始终显式可见
// （符合项目规则：不把同步藏进难以解释的 helper）。
class UploadHelper
{
public:
    // 把 data 上传到 destination（Default Heap buffer）。
    // 在 cmd 中记录：
    //   1. destination: COMMON -> COPY_DEST
    //   2. CopyBufferRegion(staging -> destination)
    //   3. destination: COPY_DEST -> finalState
    // 返回 Upload Heap staging buffer；调用者必须在 GPU 完成 copy 之后才释放它
    // （通常用 fence 等待后随作用域释放）。
    static ComPtr<ID3D12Resource> Upload(ID3D12Device* device,
                                         ID3D12GraphicsCommandList* cmd,
                                         GPUBuffer& destination,
                                         const void* data,
                                         std::uint64_t sizeBytes,
                                         D3D12_RESOURCE_STATES finalState);
};
