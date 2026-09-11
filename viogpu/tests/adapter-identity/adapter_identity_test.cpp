// OS peers are controlled; identity publication, readiness and serialization
// below are extracted from the production driver, using its real wire types.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <cstddef>
#include "viogpu_adapter_identity.h"
using UINT = uint32_t;
using ULONG = uint32_t;
using LONG = int32_t;
using LONG64 = int64_t;
using ULONGLONG = uint64_t;
using UCHAR = uint8_t;
using CHAR = int8_t;
using VOID = void;
using BOOLEAN = bool;
using NTSTATUS = int32_t;
#include "viogpu_3d_wire.h"
#ifndef _IRQL_requires_max_
#define _IRQL_requires_max_(level)
#endif
#define PAGED_CODE() ((void)0)
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#ifndef _Out_
#define _Out_
#endif
#define FIELD_OFFSET(type, member) offsetof(type, member)
constexpr bool TRUE = true, FALSE = false;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_GRAPHICS_DRIVER_MISMATCH = -1, STATUS_DEVICE_NOT_READY = -2;
constexpr LONG VIOGPU_READINESS_FAIL_RUNDOWN = 1, VIOGPU_READINESS_FAIL_RESET_REQUESTED = 2,
               VIOGPU_READINESS_FAIL_NO_HW_DEVICE = 4;
struct LUID
{
    uint32_t LowPart;
    int32_t HighPart;
};
void RtlZeroMemory(void *destination, size_t size)
{
    std::memset(destination, 0, size);
}
void RtlCopyMemory(void *destination, const void *source, size_t size)
{
    std::memcpy(destination, source, size);
}
LONG InterlockedExchange(volatile LONG *target, LONG value)
{
    LONG previous = *target;
    *target = value;
    return previous;
}
LONG64 InterlockedExchange64(volatile LONG64 *target, LONG64 value)
{
    LONG64 previous = *target;
    *target = value;
    return previous;
}
LONG64 InterlockedCompareExchange64(volatile LONG64 *target, LONG64 value, LONG64 expected)
{
    LONG64 previous = *target;
    if (previous == expected)
    {
        *target = value;
    }
    return previous;
}
struct Rundown
{
    bool allowed = true;
    unsigned acquired = 0;
};
bool ExAcquireRundownProtection(Rundown *r)
{
    if (!r->allowed)
    {
        return false;
    }
    r->acquired++;
    return true;
}
void ExReleaseRundownProtection(Rundown *r)
{
    r->acquired--;
}
struct VioGpuAdapter
{
    bool ready = true;
    ULONGLONG generation = 13;
    GPU_CAPSET_DRM cap = {};
    std::function<void()> query_hook;
    BOOLEAN QueryNativeContextReadiness(GPU_CAPSET_DRM *output, UINT *, UINT *, ULONGLONG *reset)
    {
        if (query_hook)
        {
            query_hook();
        }
        *output = cap;
        if (reset)
        {
            *reset = generation;
        }
        return ready;
    }
};
struct VioGpuDod
{
    Rundown m_HardwareOperations;
    VioGpuAdapter *m_pHWDevice = nullptr;
    volatile LONG m_DodReadinessFailMask = 0;
    volatile LONG64 m_RuntimeAdapterLuid = 0;
    bool resetting = false;
    unsigned diagnostics = 0;
    bool IsHardwareResetRequested() const
    {
        return resetting;
    }
    void RecordNativeReadinessDiagnostic()
    {
        diagnostics++;
    }
    VOID SetNativeAdapterLuid(const LUID *adapterLuid);
    BOOLEAN QueryNativeContextReadiness(GPU_CAPSET_DRM *, UINT *, UINT *, ULONGLONG *, LUID * = nullptr);
};
struct DXGKARG_QUERYADAPTERINFO
{
    const void *pInputData = nullptr;
    UINT InputDataSize = 0;
    void *pOutputData = nullptr;
    UINT OutputDataSize = 0;
};

// INSERT_PRODUCTION

static unsigned checks = 0, failures = 0;
static void check(bool passed, const char *name)
{
    checks++;
    if (!passed)
    {
        failures++;
        std::fprintf(stderr, "FAIL %s\n", name);
    }
}

int main()
{
    VioGpuAdapter hardware;
    hardware.cap.version_major = 1;
    hardware.cap.version_minor = 12;
    hardware.cap.msm.chip_id = 0x44050000;
    hardware.cap.msm.has_cached_coherent = 1;
    hardware.cap.msm.has_raytracing = VIRTGPU_CAP_BOOL_TRUE;
    hardware.cap.msm.gmem_size = 12 * 1024 * 1024;
    VioGpuDod adapter;
    adapter.m_pHWDevice = &hardware;
    LUID identity = {0x1234abcd, static_cast<int32_t>(0xfedc9876u)};
    VIOGPU_WDDM_ADAPTER_INFO prefix = {};
    DXGKARG_QUERYADAPTERINFO query;
    query.pOutputData = &prefix;
    query.OutputDataSize = sizeof(prefix);
    check(QueryUmdPrivateInfo(&adapter, &query) == STATUS_SUCCESS, "legacy query needs no new identity");
    check(prefix.Header.Size == 128 && prefix.Header.Version == 0 && prefix.HasRayTracing == 1,
          "legacy ABI and ray-query field preserved");
    adapter.SetNativeAdapterLuid(&identity);
    VIOGPU_WDDM_ADAPTER_INFO_WITH_IDENTITY expected = {};
    expected.AdapterInfo = prefix;
    expected.Identity = {0x44494c56, 1, 32, 1, identity.LowPart, static_cast<uint32_t>(identity.HighPart), 1, 0};
    std::array<unsigned char, 208> data;
    for (UINT size = 0; size <= 176; size++)
    {
        data.fill(0xa5);
        query.pOutputData = data.data() + 16;
        query.OutputDataSize = size;
        NTSTATUS status = QueryUmdPrivateInfo(&adapter, &query);
        size_t written = size < 16 ? 0 : size >= 160 ? 160 : size < 128 ? size : 128;
        bool passed = status == (size < 16 ? STATUS_GRAPHICS_DRIVER_MISMATCH : STATUS_SUCCESS);
        passed &= std::memcmp(data.data() + 16, &expected, written) == 0;
        for (size_t i = 0; i < data.size(); i++)
        {
            if (i < 16 || i >= 16 + written)
            {
                passed &= data[i] == 0xa5;
            }
        }
        check(passed, "bounded prefix or complete trailer with guards");
    }
    query.OutputDataSize = UINT32_MAX;
    data.fill(0xa5);
    check(QueryUmdPrivateInfo(&adapter,
                              &query) == STATUS_SUCCESS && std::memcmp(data.data() + 16, &expected, 160) == 0 &&
                                                                                                              data[176] == 0xa5,
          "huge declared output still writes only 160");
    query.OutputDataSize = 160;
    auto rejects_without_writes = [&]() {
        data.fill(0xa5);
        NTSTATUS status = QueryUmdPrivateInfo(&adapter, &query);
        bool untouched = true;
        for (unsigned char c : data)
        {
            untouched &= c == 0xa5;
        }
        return status != STATUS_SUCCESS && untouched && adapter.m_HardwareOperations.acquired == 0;
    };
    adapter.SetNativeAdapterLuid(nullptr);
    check(rejects_without_writes(), "stop or failed start invalidates identity");
    LUID zero = {};
    adapter.SetNativeAdapterLuid(&zero);
    check(rejects_without_writes(), "zero Windows identity rejected");
    adapter.SetNativeAdapterLuid(&identity);
    adapter.resetting = true;
    check(rejects_without_writes(), "identity hidden during GPU reset");
    adapter.resetting = false;
    hardware.generation++;
    check(QueryUmdPrivateInfo(&adapter, &query) == STATUS_SUCCESS, "identity survives same-adapter reset");
    VIOGPU_WDDM_ADAPTER_INFO_WITH_IDENTITY after_reset;
    std::memcpy(&after_reset, data.data() + 16, sizeof(after_reset));
    check(after_reset.AdapterInfo.ResetGeneration == 14 && std::memcmp(&after_reset.Identity,
                                                                       &expected.Identity,
                                                                       32) == 0,
          "reset generation is independent of Windows identity");
    hardware.ready = false;
    check(rejects_without_writes(), "unready hardware rejected");
    hardware.ready = true;
    adapter.m_HardwareOperations.allowed = false;
    check(rejects_without_writes(), "rundown rejects identity query");
    adapter.m_HardwareOperations.allowed = true;
    adapter.m_pHWDevice = nullptr;
    check(rejects_without_writes(), "removed hardware rejected");
    adapter.m_pHWDevice = &hardware;
    hardware.query_hook = [&]() { adapter.SetNativeAdapterLuid(nullptr); };
    check(rejects_without_writes(), "stop between readiness and identity read");
    hardware.query_hook = nullptr;
    identity.LowPart = 0x76543210;
    adapter.SetNativeAdapterLuid(&identity);
    check(QueryUmdPrivateInfo(&adapter, &query) == STATUS_SUCCESS, "new start publishes new OS identity");
    std::memcpy(&after_reset, data.data() + 16, sizeof(after_reset));
    check(after_reset.Identity.AdapterLuidLowPart == identity.LowPart, "no stale LUID after new start");
    hardware.cap.msm.has_raytracing = 2;
    check(rejects_without_writes(), "invalid existing capability still rejected");
    hardware.cap.msm.has_raytracing = VIRTGPU_CAP_BOOL_TRUE;
    hardware.generation = 0;
    check(rejects_without_writes(), "zero reset generation rejected");
    hardware.generation = 14;
    query.pInputData = &identity;
    check(rejects_without_writes(), "runtime callback is output-only");
    query.pInputData = nullptr;
    query.InputDataSize = 4;
    check(rejects_without_writes(), "nonzero input size rejected");
    query.InputDataSize = 0;
    check(QueryUmdPrivateInfo(nullptr, &query) == STATUS_GRAPHICS_DRIVER_MISMATCH, "null adapter rejected");
    query.pOutputData = nullptr;
    check(QueryUmdPrivateInfo(&adapter, &query) == STATUS_GRAPHICS_DRIVER_MISMATCH, "null output rejected");
    check(adapter.m_HardwareOperations.acquired == 0, "all queries release hardware rundown");
    std::printf("adapter identity: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
