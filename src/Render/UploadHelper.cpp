#include "Render/UploadHelper.h"

#include <cstring>

namespace
{

// 手写 ResourceBarrier，显式写出转换前后状态。
void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);
}

} // namespace

ComPtr<ID3D12Resource> UploadHelper::Upload(ID3D12Device* device,
                                            ID3D12GraphicsCommandList* cmd,
                                            GPUBuffer& destination,
                                            const void* data,
                                            std::uint64_t sizeBytes,
                                            D3D12_RESOURCE_STATES finalState)
{
    // 1. 创建 Upload Heap staging buffer。
    //    Upload Heap 位于 CPU 可写的系统内存，用于「暂存」要交给 GPU 的数据。
    //    惯例初始状态 GENERIC_READ（作为 GPU 只读 / copy source）。
    GPUBuffer staging;
    if (!staging.Initialize(device, sizeBytes, D3D12_HEAP_TYPE_UPLOAD,
                            D3D12_RESOURCE_STATE_GENERIC_READ))
    {
        return nullptr;
    }

    // 2. CPU 把数据写进 staging（Map -> memcpy -> Unmap）。
    //    readRange = {0,0} 表示 CPU 只写不读，避免不必要的 GPU cache flush。
    D3D12_RANGE readRange = { 0, 0 };
    void* mapped = staging.Map(0, &readRange);
    if (mapped == nullptr)
    {
        return nullptr;
    }
    std::memcpy(mapped, data, static_cast<size_t>(sizeBytes));
    staging.Unmap(0, nullptr);

    // 3. 记录命令：状态转换 + 拷贝 + 状态转换。
    //    destination（Default Heap）从 COMMON 转 COPY_DEST 以接收拷贝，
    //    拷贝完成后再转到最终用途状态（finalState，如 VERTEX_AND_CONSTANT_BUFFER）。
    Transition(cmd, destination.GetResource(),
               destination.GetState(), D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyBufferRegion(destination.GetResource(), 0,
                          staging.GetResource(), 0, sizeBytes);
    Transition(cmd, destination.GetResource(),
               D3D12_RESOURCE_STATE_COPY_DEST, finalState);
    destination.SetState(finalState);

    // 4. 返回 staging 的引用（ComPtr 增加引用计数，staging 局部析构后资源仍存活），
    //    由调用者在 GPU 完成 copy 后释放。
    return staging.GetResource();
}
