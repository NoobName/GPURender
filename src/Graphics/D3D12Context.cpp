#include "Graphics/D3D12Context.h"

#include <dxgidebug.h> // IDXGIDebug / DXGIGetDebugInterface1

#include <iostream>
#include <iterator>
#include <vector>

namespace
{

// 将 DXGI 返回的宽字符串转换为 UTF-8 窄字符串（便于 std::cout 输出）。
std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty())
    {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

const char* FeatureLevelToString(D3D_FEATURE_LEVEL level)
{
    switch (level)
    {
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_12_0: return "12_0";
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    case D3D_FEATURE_LEVEL_12_2: return "12_2";
    default: return "unknown";
    }
}

const char* ShaderModelToString(D3D_SHADER_MODEL model)
{
    switch (model)
    {
    case D3D_SHADER_MODEL_5_1: return "5.1";
    case D3D_SHADER_MODEL_6_0: return "6.0";
    case D3D_SHADER_MODEL_6_1: return "6.1";
    case D3D_SHADER_MODEL_6_2: return "6.2";
    case D3D_SHADER_MODEL_6_3: return "6.3";
    case D3D_SHADER_MODEL_6_4: return "6.4";
    case D3D_SHADER_MODEL_6_5: return "6.5";
    case D3D_SHADER_MODEL_6_6: return "6.6";
    case D3D_SHADER_MODEL_6_7: return "6.7";
    case D3D_SHADER_MODEL_6_8: return "6.8";
    case D3D_SHADER_MODEL_6_9: return "6.9";
    default: return "unknown";
    }
}

const char* ResourceBindingTierToString(D3D12_RESOURCE_BINDING_TIER tier)
{
    switch (tier)
    {
    case D3D12_RESOURCE_BINDING_TIER_1: return "Tier 1";
    case D3D12_RESOURCE_BINDING_TIER_2: return "Tier 2";
    case D3D12_RESOURCE_BINDING_TIER_3: return "Tier 3";
    default: return "unknown";
    }
}

const char* MeshShaderTierToString(D3D12_MESH_SHADER_TIER tier)
{
    switch (tier)
    {
    case D3D12_MESH_SHADER_TIER_NOT_SUPPORTED: return "Not Supported";
    case D3D12_MESH_SHADER_TIER_1: return "Tier 1";
    default: return "unknown";
    }
}

const char* RaytracingTierToString(D3D12_RAYTRACING_TIER tier)
{
    switch (tier)
    {
    case D3D12_RAYTRACING_TIER_NOT_SUPPORTED: return "Not Supported";
    case D3D12_RAYTRACING_TIER_1_0: return "1.0";
    case D3D12_RAYTRACING_TIER_1_1: return "1.1";
    case D3D12_RAYTRACING_TIER_1_2: return "1.2";
    default: return "unknown";
    }
}

} // namespace

bool D3D12Context::Initialize()
{
    if (!EnableDebugLayer())
    {
        return false;
    }
    if (!CreateFactory())
    {
        return false;
    }
    if (!SelectAdapter())
    {
        return false;
    }
    if (!CreateDevice())
    {
        return false;
    }
    return true;
}

void D3D12Context::Shutdown()
{
    // 1. 导出 Debug Layer 捕获的消息（device 仍存活时可用）
    DumpDebugLayerMessages();

    // 2. 按依赖逆序释放 COM 对象：InfoQueue -> Device -> Adapter -> Factory
    m_infoQueue.Reset();
    m_device.Reset();
    m_adapter.Reset();
    m_factory.Reset();

    // 3. 报告仍存活的对象（若上面有遗漏，Debug Layer 会在这里列出）
    ReportLiveObjects();

    // 4. 最后释放 Debug 控制器，关闭 Debug Layer
    m_debugController.Reset();
}

bool D3D12Context::EnableDebugLayer()
{
#if defined(_DEBUG)
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&m_debugController))))
    {
        std::cerr << "[D3D12] Failed to obtain ID3D12Debug. "
                     "Make sure the Windows 'Graphics Tools' optional feature is installed.\n";
        return false;
    }
    m_debugController->EnableDebugLayer();
    std::cout << "[D3D12] Debug Layer enabled.\n";
#endif
    return true;
}

bool D3D12Context::CreateFactory()
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags |= DXGI_CREATE_FACTORY_DEBUG; // 让 DXGI 也进入 debug 模式，便于校验工厂/枚举用法
#endif

    const HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_factory));
    if (FAILED(hr))
    {
        std::cerr << "[DXGI] CreateDXGIFactory2 failed: 0x" << std::hex << hr << std::dec << "\n";
        return false;
    }
    return true;
}

bool D3D12Context::SelectAdapter()
{
    // 优先：用 IDXGIFactory6::EnumAdapterByGpuPreference 让系统按"高性能"排序，
    // 直接得到最适合渲染的硬件 GPU。
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(m_factory.As(&factory6)))
    {
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT hr = factory6->EnumAdapterByGpuPreference(
                i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
            if (hr == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(hr))
            {
                continue;
            }

            DXGI_ADAPTER_DESC1 desc = {};
            adapter->GetDesc1(&desc);

            // 跳过软件适配器（WARP），只要硬件 GPU
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                continue;
            }

            m_adapter = adapter;
            m_adapterDesc = desc;
            return true;
        }
    }

    // 回退：逐项枚举，跳过软件适配器，选择独立显存最大的硬件适配器。
    ComPtr<IDXGIAdapter1> bestAdapter;
    DXGI_ADAPTER_DESC1 bestDesc = {};
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = m_factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        if (FAILED(hr))
        {
            continue;
        }

        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            continue;
        }

        if (desc.DedicatedVideoMemory > bestDesc.DedicatedVideoMemory)
        {
            bestAdapter = adapter;
            bestDesc = desc;
        }
    }

    if (bestAdapter == nullptr)
    {
        std::cerr << "[DXGI] No hardware adapter found.\n";
        return false;
    }

    m_adapter = bestAdapter;
    m_adapterDesc = bestDesc;
    return true;
}

bool D3D12Context::CreateDevice()
{
    // minFeatureLevel 取 11_0 以兼容更多硬件；
    // 设备实际支持的最高 Feature Level 稍后由 CheckFeatureSupport 查询。
    const D3D_FEATURE_LEVEL minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    const HRESULT hr = D3D12CreateDevice(m_adapter.Get(), minFeatureLevel, IID_PPV_ARGS(&m_device));
    if (FAILED(hr))
    {
        std::cerr << "[D3D12] D3D12CreateDevice failed: 0x" << std::hex << hr << std::dec << "\n";
        return false;
    }

    // Debug 构建下获取 InfoQueue，用于收集/导出 Debug Layer 消息。
    m_device.As(&m_infoQueue);
    if (m_infoQueue != nullptr)
    {
        // 遇到错误时在调试器中断下，便于定位（无调试器时无副作用）
        m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
    }

    QueryCapabilities();
    PrintAdapterInfo();
    return true;
}

void D3D12Context::QueryCapabilities()
{
    D3D12Capabilities& cap = m_capabilities;

    cap.adapterName = WideToUtf8(m_adapterDesc.Description);
    cap.dedicatedVideoMemoryBytes = m_adapterDesc.DedicatedVideoMemory;

    // 最高 Feature Level
    {
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {};
        fl.NumFeatureLevels = static_cast<UINT>(std::size(levels));
        fl.pFeatureLevelsRequested = levels;
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl))))
        {
            cap.maxFeatureLevel = fl.MaxSupportedFeatureLevel;
        }
    }

    // D3D12_OPTIONS：资源绑定层级等
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS opts = {};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts))))
        {
            cap.resourceBindingTier = opts.ResourceBindingTier;
        }
    }

    // Shader Model
    // HighestShaderModel 是 _Inout_ 参数：调用前先请求最高值，调用后返回硬件实际支持值。
    {
        D3D12_FEATURE_DATA_SHADER_MODEL sm = {};
        sm.HighestShaderModel = D3D_HIGHEST_SHADER_MODEL;
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
        {
            cap.maxShaderModel = sm.HighestShaderModel;
        }
    }

    // Mesh Shader（后续 Meshlet Milestone 依赖）
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7))))
        {
            cap.meshShaderTier = o7.MeshShaderTier;
        }
    }

    // Raytracing（预留）
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))))
        {
            cap.raytracingTier = o5.RaytracingTier;
        }
    }

    // 架构：是否 Tile-Based / UMA
    {
        D3D12_FEATURE_DATA_ARCHITECTURE arch = {};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch))))
        {
            cap.tileBasedRenderer = arch.TileBasedRenderer != 0;
            cap.umaArchitecture = arch.UMA != 0;
        }
    }

    m_featureLevel = cap.maxFeatureLevel;
}

void D3D12Context::PrintAdapterInfo()
{
    const D3D12Capabilities& cap = m_capabilities;

    std::cout << "\n================ GPU Info ================\n";
    std::cout << "  Adapter Name          : " << cap.adapterName << "\n";
    std::cout << "  Dedicated Video Memory: "
              << (cap.dedicatedVideoMemoryBytes / (1024ull * 1024ull)) << " MB\n";
    std::cout << "  Max Feature Level     : " << FeatureLevelToString(cap.maxFeatureLevel) << "\n";
    std::cout << "  Max Shader Model      : " << ShaderModelToString(cap.maxShaderModel) << "\n";
    std::cout << "  Resource Binding Tier : " << ResourceBindingTierToString(cap.resourceBindingTier) << "\n";
    std::cout << "  Mesh Shader Tier      : " << MeshShaderTierToString(cap.meshShaderTier) << "\n";
    std::cout << "  Raytracing Tier       : " << RaytracingTierToString(cap.raytracingTier) << "\n";
    std::cout << "  Tile-Based Renderer   : " << (cap.tileBasedRenderer ? "yes" : "no") << "\n";
    std::cout << "  UMA Architecture      : " << (cap.umaArchitecture ? "yes" : "no") << "\n";
    std::cout << "==========================================\n\n";
}

void D3D12Context::DumpDebugLayerMessages()
{
    if (m_infoQueue == nullptr)
    {
        return;
    }

    const UINT64 count = m_infoQueue->GetNumStoredMessages();
    if (count == 0)
    {
        std::cout << "[D3D12] Debug Layer: no stored messages (clean).\n";
        return;
    }

    UINT64 errorCount = 0;
    UINT64 warningCount = 0;
    UINT64 infoCount = 0;

    for (UINT64 i = 0; i < count; ++i)
    {
        SIZE_T length = 0;
        m_infoQueue->GetMessage(i, nullptr, &length);

        std::vector<BYTE> buffer(static_cast<size_t>(length));
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (FAILED(m_infoQueue->GetMessage(i, message, &length)))
        {
            continue;
        }

        switch (message->Severity)
        {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        case D3D12_MESSAGE_SEVERITY_ERROR:
            ++errorCount;
            std::cerr << "  [ERROR] ID=" << message->ID << ": " << message->pDescription << "\n";
            break;
        case D3D12_MESSAGE_SEVERITY_WARNING:
            ++warningCount;
            std::cout << "  [WARNING] ID=" << message->ID << ": " << message->pDescription << "\n";
            break;
        default:
            ++infoCount;
            break;
        }
    }

    std::cout << "[D3D12] Debug Layer summary: " << errorCount << " error(s), "
              << warningCount << " warning(s), " << infoCount << " info/message(s).\n";
}

void D3D12Context::ReportLiveObjects()
{
#if defined(_DEBUG)
    ComPtr<IDXGIDebug> dxgiDebug;
    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
    {
        // 输出到调试器（VS Output / DebugView），不会出现在控制台。
        dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
    }
#endif
}
