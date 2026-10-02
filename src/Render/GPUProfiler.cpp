#include "Render/GPUProfiler.h"

#include <iostream>

bool GPUProfiler::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    if (FAILED(queue->GetTimestampFrequency(&m_frequency)) || m_frequency == 0)
    {
        std::cerr << "[GPUProfiler] GetTimestampFrequency failed.\n";
        return false;
    }

    // 查询堆：TIMESTAMP 类型。槽位按帧切片，每帧 kTimestampsPerFrame 个。
    D3D12_QUERY_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = kTimestampsPerFrame * kFrameRingSize;
    heapDesc.NodeMask = 0;
    if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_queryHeap))))
    {
        std::cerr << "[GPUProfiler] CreateQueryHeap failed.\n";
        return false;
    }

    // readback 缓冲：每个时间戳 8 字节。
    // 用 READBACK Heap，因为它只被 GPU 写、CPU 读。
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeof(std::uint64_t) * kTimestampsPerFrame * kFrameRingSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readbackBuffer))))
    {
        std::cerr << "[GPUProfiler] Failed to create readback buffer.\n";
        return false;
    }

    m_initialized = true;
    std::cout << "[GPUProfiler] Ready: timestamp frequency = " << m_frequency
              << " ticks/s (" << (1e9 / static_cast<double>(m_frequency))
              << " ns per tick), " << kTimestampsPerFrame << " timestamps/frame\n";
    return true;
}

void GPUProfiler::WriteTimestamp(ID3D12GraphicsCommandList* cmd, UINT frameIndex, UINT slot)
{
    if (!m_initialized || frameIndex >= kFrameRingSize || slot >= kTimestampsPerFrame)
    {
        return;
    }
    cmd->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                  frameIndex * kTimestampsPerFrame + slot);
}

void GPUProfiler::Resolve(ID3D12GraphicsCommandList* cmd, UINT frameIndex)
{
    if (!m_initialized || frameIndex >= kFrameRingSize)
    {
        return;
    }
    cmd->ResolveQueryData(
        m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
        frameIndex * kTimestampsPerFrame, kTimestampsPerFrame,
        m_readbackBuffer.Get(),
        static_cast<UINT64>(frameIndex) * kTimestampsPerFrame * sizeof(std::uint64_t));
}

bool GPUProfiler::ReadbackCompletedFrame(UINT laggedFrameIndex,
                                         double& outCullMs,
                                         double& outDepthPassMs,
                                         double& outMainPassMs)
{
    if (!m_initialized || laggedFrameIndex >= kFrameRingSize)
    {
        return false;
    }

    const UINT64 offset =
        static_cast<UINT64>(laggedFrameIndex) * kTimestampsPerFrame * sizeof(std::uint64_t);

    // Map 只声明「我要读这一段」。若 GPU 恰好还在写它，Map 会阻塞 ——
    // 但调用方保证这一帧是 kFrameCount 帧之前的，早已完成。
    D3D12_RANGE range = { static_cast<SIZE_T>(offset),
                          static_cast<SIZE_T>(offset + sizeof(std::uint64_t) * kTimestampsPerFrame) };
    void* mapped = nullptr;
    if (FAILED(m_readbackBuffer->Map(0, &range, &mapped)))
    {
        return false;
    }

    const std::uint64_t* stamps = reinterpret_cast<const std::uint64_t*>(
        static_cast<const std::uint8_t*>(mapped) + offset);

    const std::uint64_t t0 = stamps[0]; // 帧开始
    const std::uint64_t t1 = stamps[1]; // 剔除 + 命令生成结束
    const std::uint64_t t2 = stamps[2]; // Depth Pass 结束
    const std::uint64_t t3 = stamps[3]; // Main Pass 结束

    m_readbackBuffer->Unmap(0, nullptr);

    // 未写入的槽位会是 0，直接判为无效
    if (t0 == 0 || t3 < t0)
    {
        return false;
    }

    const double toMs = 1000.0 / static_cast<double>(m_frequency);
    outCullMs = (t1 >= t0) ? static_cast<double>(t1 - t0) * toMs : 0.0;
    outDepthPassMs = (t2 >= t1) ? static_cast<double>(t2 - t1) * toMs : 0.0;
    outMainPassMs = (t3 >= t2) ? static_cast<double>(t3 - t2) * toMs : 0.0;
    return true;
}
