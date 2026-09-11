// Read-only Advanced Color observations; no HDR mode request or GPU workload.
#pragma once
#include <dxgi1_6.h>
#include <cmath>

static void JsonFloat(float value)
{
    if (std::isfinite(value))
    {
        std::printf("%.6g", static_cast<double>(value));
    }
    else
    {
        std::printf("null");
    }
}

static void DisplayColorSnapshot(LUID luid)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG status = ERROR_INSUFFICIENT_BUFFER;
    for (unsigned retry = 0; retry < 3 && status == ERROR_INSUFFICIENT_BUFFER; ++retry)
    {
        UINT32 pathCount = 0, modeCount = 0;
        status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (status != ERROR_SUCCESS)
        {
            break;
        }
        if (pathCount > 1024 || modeCount > 4096)
        {
            status = ERROR_INVALID_DATA;
            break;
        }
        paths.resize(pathCount);
        modes.resize(modeCount);
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (status == ERROR_SUCCESS)
        {
            paths.resize(pathCount);
        }
    }
    std::printf("\"display_color\":{\"win32_status\":%ld,\"targets\":[", status);
    bool first = true;
    if (status == ERROR_SUCCESS)
    {
        for (const auto &path : paths)
        {
            if (path.targetInfo.adapterId.LowPart != luid.LowPart ||
                path.targetInfo.adapterId.HighPart != luid.HighPart)
            {
                continue;
            }
            if (!first)
            {
                std::putchar(',');
            }
            first = false;
            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info = {};
            info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
            info.header.size = sizeof(info);
            info.header.adapterId = luid;
            info.header.id = path.targetInfo.id;
            const LONG result = DisplayConfigGetDeviceInfo(&info.header);
            std::printf("{\"target_id\":%u,\"win32_status\":%ld", path.targetInfo.id, result);
            if (result == ERROR_SUCCESS)
            {
                std::printf(",\"advanced_color_supported\":%u,\"advanced_color_enabled\":%u,"
                            "\"wide_color_enforced\":%u,\"advanced_color_force_disabled\":%u,"
                            "\"bits_per_channel\":%u,\"color_encoding\":%u",
                            info.advancedColorSupported,
                            info.advancedColorEnabled,
                            info.wideColorEnforced,
                            info.advancedColorForceDisabled,
                            info.bitsPerColorChannel,
                            static_cast<UINT>(info.colorEncoding));
            }
            std::putchar('}');
        }
    }
    std::printf("],\"dxgi_outputs\":[");
    IDXGIFactory1 *factory = nullptr;
    first = true;
    HRESULT factoryResult = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory));
    if (SUCCEEDED(factoryResult))
    {
        for (UINT index = 0; index < 64; ++index)
        {
            IDXGIAdapter1 *adapter = nullptr;
            if (FAILED(factory->EnumAdapters1(index, &adapter)))
            {
                break;
            }
            DXGI_ADAPTER_DESC1 description = {};
            const bool matches = SUCCEEDED(adapter->GetDesc1(&description)) &&
                                 description.AdapterLuid.LowPart == luid.LowPart &&
                                 description.AdapterLuid.HighPart == luid.HighPart;
            if (matches)
            {
                for (UINT outputIndex = 0; outputIndex < 64; ++outputIndex)
                {
                    IDXGIOutput *output = nullptr;
                    if (FAILED(adapter->EnumOutputs(outputIndex, &output)))
                    {
                        break;
                    }
                    IDXGIOutput6 *output6 = nullptr;
                    HRESULT result = output->QueryInterface(__uuidof(IDXGIOutput6),
                                                            reinterpret_cast<void **>(&output6));
                    DXGI_OUTPUT_DESC1 desc = {};
                    if (SUCCEEDED(result))
                    {
                        result = output6->GetDesc1(&desc);
                    }
                    if (!first)
                    {
                        std::putchar(',');
                    }
                    first = false;
                    std::printf("{\"output_index\":%u,\"hresult\":\"0x%08lx\"",
                                outputIndex,
                                static_cast<ULONG>(result));
                    if (SUCCEEDED(result))
                    {
                        std::printf(",\"device_name\":");
                        JsonString(desc.DeviceName);
                        std::printf(",\"attached\":%s,\"bits_per_channel\":%u,\"color_space\":%u,\"min_luminance\":",
                                    desc.AttachedToDesktop ? "true" : "false",
                                    desc.BitsPerColor,
                                    static_cast<UINT>(desc.ColorSpace));
                        JsonFloat(desc.MinLuminance);
                        std::printf(",\"max_luminance\":");
                        JsonFloat(desc.MaxLuminance);
                        std::printf(",\"max_full_frame_luminance\":");
                        JsonFloat(desc.MaxFullFrameLuminance);
                    }
                    std::putchar('}');
                    if (output6)
                    {
                        output6->Release();
                    }
                    output->Release();
                }
            }
            adapter->Release();
            if (matches)
            {
                break;
            }
        }
        factory->Release();
    }
    std::printf("],\"dxgi_factory_hresult\":\"0x%08lx\"},", static_cast<ULONG>(factoryResult));
}
