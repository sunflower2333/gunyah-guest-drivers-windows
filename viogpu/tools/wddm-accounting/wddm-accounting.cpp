// Read-only OS accounting snapshot. No driver installation or workload launch.
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>

static void JsonString(const wchar_t *value)
{
    std::putchar('"');
    for (; *value; ++value)
    {
        if (*value >= 0x20 && *value < 0x7f && *value != L'"' && *value != L'\\')
        {
            std::putchar(static_cast<char>(*value));
        }
        else
        {
            std::printf("\\u%04x", static_cast<unsigned int>(*value));
        }
    }
    std::putchar('"');
}

#include "display-color.h"

static void Status(NTSTATUS status)
{
    std::printf("\"ntstatus\":\"0x%08lx\"", static_cast<unsigned long>(status));
}

template <typename T> static NTSTATUS Query(D3DKMT_HANDLE adapter, KMTQUERYADAPTERINFOTYPE type, T *data)
{
    D3DKMT_QUERYADAPTERINFO query = {};
    query.hAdapter = adapter;
    query.Type = type;
    query.pPrivateDriverData = data;
    query.PrivateDriverDataSize = sizeof(*data);
    return D3DKMTQueryAdapterInfo(&query);
}

static void Description(LUID luid)
{
    IDXGIFactory1 *factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
    {
        for (UINT index = 0; index < 64; ++index)
        {
            IDXGIAdapter1 *adapter = nullptr;
            if (FAILED(factory->EnumAdapters1(index, &adapter)))
            {
                break;
            }
            DXGI_ADAPTER_DESC1 description = {};
            const HRESULT status = adapter->GetDesc1(&description);
            adapter->Release();
            if (SUCCEEDED(status) && description.AdapterLuid.LowPart == luid.LowPart &&
                description.AdapterLuid.HighPart == luid.HighPart)
            {
                std::printf("\"description\":");
                JsonString(description.Description);
                std::printf(",\"vendor_id\":%u,\"device_id\":%u,", description.VendorId, description.DeviceId);
                factory->Release();
                return;
            }
        }
        factory->Release();
    }
    std::printf("\"description\":null,");
}

static void NodeStatistics(LUID luid, UINT node, HANDLE process)
{
    D3DKMT_QUERYSTATISTICS query = {};
    query.Type = process ? D3DKMT_QUERYSTATISTICS_PROCESS_NODE : D3DKMT_QUERYSTATISTICS_NODE;
    query.AdapterLuid = luid;
    query.hProcess = process;
    if (process)
    {
        query.QueryProcessNode.NodeId = node;
    }
    else
    {
        query.QueryNode.NodeId = node;
    }
    NTSTATUS status = D3DKMTQueryStatistics(&query);
    std::putchar('{');
    Status(status);
    if (status >= 0)
    {
        const auto &data = process ? query.QueryResult.ProcessNodeInformation
                                   : query.QueryResult.NodeInformation.GlobalInformation;
        std::printf(",\"running_time_100ns\":%lld,\"context_switches\":%llu",
                    data.RunningTime.QuadPart,
                    static_cast<unsigned long long>(data.ContextSwitch));
    }
    std::putchar('}');
}

static void AdapterSnapshot(const D3DKMT_ADAPTERINFO &adapter, bool statistics, HANDLE process)
{
    std::printf("{\"luid\":\"0x%08lx_0x%08lx\",",
                static_cast<unsigned long>(adapter.AdapterLuid.HighPart),
                adapter.AdapterLuid.LowPart);
    Description(adapter.AdapterLuid);
    DisplayColorSnapshot(adapter.AdapterLuid);
    D3DKMT_DRIVERVERSION version = {};
    NTSTATUS status = Query(adapter.hAdapter, KMTQAITYPE_DRIVERVERSION, &version);
    std::printf("\"driver_model\":{");
    Status(status);
    if (status >= 0)
    {
        std::printf(",\"raw\":%u", static_cast<unsigned int>(version));
    }
    std::printf("},\"adapter_type\":{");
    D3DKMT_ADAPTERTYPE type = {};
    status = Query(adapter.hAdapter, KMTQAITYPE_ADAPTERTYPE, &type);
    Status(status);
    if (status >= 0)
    {
        std::printf(",\"render\":%u,\"display\":%u,\"software\":%u",
                    type.RenderSupported,
                    type.DisplaySupported,
                    type.SoftwareDevice);
    }
    std::printf("},\"segment_sizes\":{");
    D3DKMT_SEGMENTSIZEINFO sizes = {};
    status = Query(adapter.hAdapter, KMTQAITYPE_GETSEGMENTSIZE, &sizes);
    Status(status);
    if (status >= 0)
    {
        std::printf(",\"dedicated_video_bytes\":%llu,\"dedicated_system_bytes\":%llu,\"shared_system_bytes\":%llu",
                    sizes.DedicatedVideoMemorySize,
                    sizes.DedicatedSystemMemorySize,
                    sizes.SharedSystemMemorySize);
    }
    std::putchar('}');

    UINT nodes = 64;
    UINT segments = 0;
    if (statistics)
    {
        // Explicit opt-in: Microsoft reserves this API for system use. Use the
        // installed WDK declarations and preserve failures; never infer zeros.
        D3DKMT_QUERYSTATISTICS query = {};
        query.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
        query.AdapterLuid = adapter.AdapterLuid;
        status = D3DKMTQueryStatistics(&query);
        std::printf(",\"adapter_statistics\":{");
        Status(status);
        if (status >= 0)
        {
            const auto &data = query.QueryResult.AdapterInformation;
            std::printf(",\"node_count\":%lu,\"segment_count\":%lu,\"tdr_count\":%lu",
                        static_cast<unsigned long>(data.NodeCount),
                        static_cast<unsigned long>(data.NbSegments),
                        static_cast<unsigned long>(data.TdrDetectedCount));
            // Bound damaged or changed ABI output instead of trusting a count.
            if (data.NodeCount <= 64 && data.NbSegments <= 64)
            {
                nodes = data.NodeCount;
                segments = data.NbSegments;
            }
            else
            {
                std::printf(",\"count_rejected\":true");
            }
        }
        std::putchar('}');
    }
    std::printf(",\"nodes\":[");
    for (UINT node = 0; node < nodes; ++node)
    {
        D3DKMT_NODEMETADATA metadata = {};
        metadata.NodeOrdinalAndAdapterIndex = node; // Physical adapter 0.
        status = Query(adapter.hAdapter, KMTQAITYPE_NODEMETADATA, &metadata);
        if (node)
        {
            std::putchar(',');
        }
        std::printf("{\"node\":%u,\"physical_adapter\":0,", node);
        Status(status);
        if (status >= 0)
        {
            std::printf(",\"engine_type\":%u,\"gpu_mmu\":%u,\"io_mmu\":%u,\"name\":",
                        static_cast<unsigned int>(metadata.NodeData.EngineType),
                        metadata.NodeData.GpuMmuSupported,
                        metadata.NodeData.IoMmuSupported);
            metadata.NodeData.FriendlyName[_countof(metadata.NodeData.FriendlyName) - 1] = 0;
            JsonString(metadata.NodeData.FriendlyName);
        }
        if (statistics && status >= 0)
        {
            std::printf(",\"global\":");
            NodeStatistics(adapter.AdapterLuid, node, nullptr);
            if (process)
            {
                std::printf(",\"process\":");
                NodeStatistics(adapter.AdapterLuid, node, process);
            }
        }
        std::putchar('}');
        if (status < 0)
        {
            break; // Record terminator; do not claim a count from failure alone.
        }
    }
    std::printf("],\"segments\":[");
    for (UINT segment = 0; segment < segments; ++segment)
    {
        if (segment)
        {
            std::putchar(',');
        }
        D3DKMT_QUERYSTATISTICS query = {};
        query.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
        query.AdapterLuid = adapter.AdapterLuid;
        query.QuerySegment.SegmentId = segment;
        status = D3DKMTQueryStatistics(&query);
        std::printf("{\"statistics_segment_index\":%u,", segment);
        Status(status);
        if (status >= 0)
        {
            const auto &data = query.QueryResult.SegmentInformation;
            std::printf(",\"aperture\":%lu,\"commit_limit_bytes\":%llu,\"committed_bytes\":%llu,\"resident_bytes\":%"
                        "llu",
                        static_cast<unsigned long>(data.Aperture),
                        data.CommitLimit,
                        data.BytesCommitted,
                        data.BytesResident);
        }
        if (process)
        {
            query = {};
            query.Type = D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT;
            query.AdapterLuid = adapter.AdapterLuid;
            query.hProcess = process;
            query.QueryProcessSegment.SegmentId = segment;
            status = D3DKMTQueryStatistics(&query);
            std::printf(",\"process\":{");
            Status(status);
            if (status >= 0)
            {
                std::printf(",\"committed_bytes\":%llu", query.QueryResult.ProcessSegmentInformation.BytesCommitted);
            }
            std::putchar('}');
        }
        std::putchar('}');
    }
    std::printf("]}");
}

int wmain(int argc, wchar_t **argv)
{
    bool statistics = false;
    const wchar_t *selected = nullptr;
    DWORD processId = 0;
    for (int index = 1; index < argc; ++index)
    {
        if (std::wcscmp(argv[index], L"--kmt-statistics") == 0)
        {
            statistics = true;
        }
        else if (std::wcscmp(argv[index], L"--luid") == 0 && index + 1 < argc)
        {
            selected = argv[++index];
        }
        else if (std::wcscmp(argv[index], L"--pid") == 0 && index + 1 < argc)
        {
            wchar_t *end = nullptr;
            processId = std::wcstoul(argv[++index], &end, 10);
            if (!processId || !end || *end)
            {
                return 2;
            }
        }
        else
        {
            std::fwprintf(stderr,
                          L"Usage: wddm-accounting.exe [--luid 0xHHHHHHHH_0xLLLLLLLL] [--pid PID] "
                          L"[--kmt-statistics]\n");
            return std::wcscmp(argv[index], L"--help") == 0 ? 0 : 2;
        }
    }
    HANDLE process = processId ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId) : nullptr;
    const DWORD processError = processId && !process ? GetLastError() : 0;
    D3DKMT_ENUMADAPTERS2 enumeration = {};
    NTSTATUS status = D3DKMTEnumAdapters2(&enumeration);
    if (status < 0 || enumeration.NumAdapters == 0 || enumeration.NumAdapters > 64)
    {
        std::printf("{\"enumeration_status\":\"0x%08lx\",\"adapter_count\":%lu}\n",
                    static_cast<unsigned long>(status),
                    static_cast<unsigned long>(enumeration.NumAdapters));
        if (process)
        {
            CloseHandle(process);
        }
        return 2;
    }
    std::vector<D3DKMT_ADAPTERINFO> adapters(enumeration.NumAdapters);
    enumeration.pAdapters = adapters.data();
    status = D3DKMTEnumAdapters2(&enumeration);
    if (status < 0 || enumeration.NumAdapters > adapters.size())
    {
        std::printf("{\"enumeration_status\":\"0x%08lx\"}\n", static_cast<unsigned long>(status));
        if (process)
        {
            CloseHandle(process);
        }
        return 2;
    }
    LARGE_INTEGER qpc = {}, frequency = {};
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&frequency);
    std::printf("{\"schema\":\"viogpu_wddm2_snapshot_v1\",\"qpc\":%lld,\"qpc_frequency\":%lld,\"pid\":%lu,\"process_"
                "open_error\":%lu,\"statistics_opt_in\":%s,\"adapters\":[",
                qpc.QuadPart,
                frequency.QuadPart,
                processId,
                processError,
                statistics ? "true" : "false");
    UINT emitted = 0;
    for (UINT index = 0; index < enumeration.NumAdapters; ++index)
    {
        const auto &adapter = adapters[index];
        wchar_t luid[32] = {};
        swprintf_s(luid,
                   L"0x%08lx_0x%08lx",
                   static_cast<unsigned long>(adapter.AdapterLuid.HighPart),
                   adapter.AdapterLuid.LowPart);
        if (!selected || _wcsicmp(selected, luid) == 0)
        {
            if (emitted++)
            {
                std::putchar(',');
            }
            AdapterSnapshot(adapter, statistics, process);
        }
        D3DKMT_CLOSEADAPTER close = {};
        close.hAdapter = adapter.hAdapter;
        D3DKMTCloseAdapter(&close);
    }
    std::printf("],\"selection_found\":%s}\n", emitted ? "true" : "false");
    if (process)
    {
        CloseHandle(process);
    }
    return emitted ? 0 : 2;
}
