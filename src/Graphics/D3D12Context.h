#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

// 汇总本机 GPU 与 D3D12 能力，供后续 Milestone 决策（可扩展）。
// 这些字段由 CheckFeatureSupport 查询得到，属于"设备能力快照"。
struct D3D12Capabilities
{
    // 设备实际支持的最高 Feature Level（决定可用特性集合）
    D3D_FEATURE_LEVEL maxFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    // 硬件支持的最高 Shader Model
    D3D_SHADER_MODEL maxShaderModel = D3D_SHADER_MODEL_5_1;
    // 资源绑定层级（决定 descriptor 数量 / 类型上限）
    D3D12_RESOURCE_BINDING_TIER resourceBindingTier = D3D12_RESOURCE_BINDING_TIER_1;
    // Mesh Shader 层级（后续 Meshlet Milestone 必需）
    D3D12_MESH_SHADER_TIER meshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
    // 光线追踪层级（预留）
    D3D12_RAYTRACING_TIER raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    // 是否 Tile-Based GPU（影响带宽优化策略）
    bool tileBasedRenderer = false;
    // 是否统一内存架构（影响资源放置策略）
    bool umaArchitecture = false;
    // Adapter 名称（已转 UTF-8）
    std::string adapterName;
    // 独立显存大小（字节）
    std::uint64_t dedicatedVideoMemoryBytes = 0;
};

// 封装 DXGI + D3D12 的初始化流程：
//   Debug Layer 启用 -> 创建 DXGI Factory -> 选择硬件 Adapter -> 创建设备 -> 查询能力。
// 本阶段不创建 CommandQueue / SwapChain / 任何渲染资源。
class D3D12Context
{
public:
    bool Initialize();
    void Shutdown();

    ID3D12Device* GetDevice() const { return m_device.Get(); }
    IDXGIFactory4* GetFactory() const { return m_factory.Get(); }
    const D3D12Capabilities& GetCapabilities() const { return m_capabilities; }

private:
    bool EnableDebugLayer();
    bool CreateFactory();
    bool SelectAdapter();
    bool CreateDevice();
    void QueryCapabilities();
    void PrintAdapterInfo();
    void DumpDebugLayerMessages();
    void ReportLiveObjects();

    // Debug 层控制器：必须在所有 D3D12 对象之前创建、最后释放
    ComPtr<ID3D12Debug> m_debugController;
    // DXGI 工厂：枚举 Adapter 的入口
    ComPtr<IDXGIFactory4> m_factory;
    // 选中的硬件 Adapter（对应物理 GPU）
    ComPtr<IDXGIAdapter1> m_adapter;
    // D3D12 逻辑设备：后续一切资源的创建入口
    ComPtr<ID3D12Device> m_device;
    // Debug Layer 消息队列（仅 Debug 构建存在）
    ComPtr<ID3D12InfoQueue> m_infoQueue;

    DXGI_ADAPTER_DESC1 m_adapterDesc = {};
    D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_11_0;
    D3D12Capabilities m_capabilities;
};
