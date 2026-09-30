#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "Scene/InstanceData.h"

using Microsoft::WRL::ComPtr;

// GPU 实例数据的运行期验证器。
//
// =============================================================================
// 为什么需要它（M9 的核心验收项）
// =============================================================================
// M9 把 InstanceData 按二进制直接写进 GPU Buffer。C++ 侧的布局有 static_assert
// 兜底，但 **HLSL 侧没有任何编译期手段**能断言自己算出的 sizeof(InstanceData)。
//
// 而布局不一致的后果是**静默的**：D3D12 不检查 StructuredBuffer 的元素步长
// 是否与写入方的步长一致，错了就只是读数错位 ——
// 典型表现是「矩阵读出来是乱的、包围球跑到别的实例身上」，且没有任何报错。
//
// 于是这里用运行期验证补上：
//   1. 让 CS 上报 HLSL 算出的 sizeof(InstanceData)，与 CPU 的比对；
//   2. 逐实例做自洽性检查（世界位置 == 包围球球心、padding == 0、仿射性等），
//      这些检查在布局错位时必然失败；
//   3. 把前 N 个实例的原始 96 字节读回来，与 CPU 数据做**逐字节 memcmp**。
//
// =============================================================================
// 资源说明
// =============================================================================
//   resultsBuffer : DEFAULT Heap + UAV，每实例一个 uint 错误码
//                   （大小 = maxInstances * 4B，100k 时约 390 KB）
//   dumpBuffer    : DEFAULT Heap + UAV，[0] = 元信息，[1..] = 前 N 个实例的原始字节
//   readbackBuffer: READBACK Heap，上面两者拷回来的落点
//
//   为什么 results 用 per-instance 数组而不是累计计数器：
//   不需要事先清零缓冲区、不需要原子操作，而且 CPU 能按 bit 定位错误类型。
//
//   HINT: UPLOAD Heap 的资源**不能**做 UAV（D3D12 的硬性限制），
//   所以这两个缓冲必须是 DEFAULT Heap + READBACK 回读。
// =============================================================================
class InstanceValidator
{
public:
    // 与 shaders/InstanceValidationCS.hlsl 中的常量保持一致
    static constexpr UINT kMaxDumpInstances = 64;
    static constexpr UINT kFloatsPerInstance = 6; // 96 / 16
    static constexpr UINT kDumpInfoFloats = 1;    // dump[0] 存元信息

    bool Initialize(ID3D12Device* device,
                    std::uint32_t maxInstances,
                    ID3D12DescriptorHeap* descriptorHeap,
                    UINT descriptorSize,
                    UINT resultsUavSlot,
                    UINT dumpUavSlot);

    // 记录验证用的 CS dispatch。调用方负责 ExecuteCommandLists + fence 等待，
    // 之后才能调用 Report（那时 GPU 已经写完 UAV）。
    //
    // 需要传入描述符堆与各槽位：根签名里的两张描述符表都要靠「堆内 GPU 句柄」定位，
    // 而句柄 = 堆起始 + 槽位 * descriptorSize。
    void Record(ID3D12GraphicsCommandList* cmd,
                ID3D12DescriptorHeap* descriptorHeap,
                UINT descriptorSize,
                UINT instanceSrvSlot,
                UINT resultsUavSlot,
                std::uint32_t instanceCount);

    // 读回结果并与 CPU 侧数据比对，把报告打印到控制台。
    // 返回 true 表示全部检查通过。
    bool Report(const std::vector<InstanceData>& cpuInstances);

private:
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;

    ComPtr<ID3D12Resource> m_resultsBuffer;  // DEFAULT + UAV
    ComPtr<ID3D12Resource> m_dumpBuffer;     // DEFAULT + UAV
    ComPtr<ID3D12Resource> m_readbackBuffer; // READBACK

    std::uint32_t m_capacity = 0;

    // readback 内的偏移（都按 256 字节对齐，避免 CopyBufferRegion 的对齐问题）
    UINT64 m_resultsOffset = 0;
    UINT64 m_dumpOffset = 0;
    UINT64 m_resultsBytes = 0;
    UINT64 m_dumpBytes = 0;

    bool m_hasResult = false;
};
