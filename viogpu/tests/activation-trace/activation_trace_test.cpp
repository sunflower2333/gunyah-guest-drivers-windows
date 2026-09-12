#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <string>
#include <vector>
#include "activation_trace.h"

#ifndef _In_
#define _In_
#endif
#define VOID void
#define CONST const
#define PAGED_CODE() ((void)0)
#define FALSE false
using UINT = unsigned int;
using ULONG = unsigned int;
using DWORD = unsigned int;
using NTSTATUS = int32_t;
using HANDLE = void *;
constexpr auto STATUS_SUCCESS = NTSTATUS(0);
constexpr auto STATUS_PENDING = NTSTATUS(0x103);
constexpr auto STATUS_OBJECT_NAME_NOT_FOUND = NTSTATUS(0xc0000034U);
constexpr auto STATUS_INTEGER_OVERFLOW = NTSTATUS(0xc0000095U);
constexpr auto STATUS_UNSUCCESSFUL = NTSTATUS(0xc0000001U);
constexpr auto STATUS_DEVICE_NOT_READY = NTSTATUS(0xc00000a3U);
constexpr unsigned PLUGPLAY_REGKEY_DRIVER = 1, KEY_QUERY_VALUE = 1, KEY_SET_VALUE = 2;
constexpr unsigned REG_BINARY = 3, Executive = 0, KernelMode = 0;
constexpr unsigned DPFLTR_DEFAULT_ID = 0, DPFLTR_ERROR_LEVEL = 0;
constexpr unsigned DXGKDDI_INTERFACE_VERSION = 0x5023;
constexpr unsigned DXGKQAITYPE_DRIVERCAPS = 1, DXGKQAITYPE_QUERYSEGMENT4 = 11;
constexpr unsigned VIOGPU_WIN7_DRIVERCAPS_SIZE = 528;
bool NT_SUCCESS(NTSTATUS status) { return status >= 0; }
void RtlZeroMemory(void *p, size_t n) { std::memset(p, 0, n); }
void DbgPrintEx(unsigned, unsigned, const char *, NTSTATUS) {}
struct UNICODE_STRING { const wchar_t *name; };
void RtlInitUnicodeString(UNICODE_STRING *s, const wchar_t *n) { s->name = n; }

unsigned checks = 0;
void check(bool ok, const char *message)
{
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct Registry
{
    std::map<std::wstring, std::vector<unsigned char>> Values;
    unsigned Writes = 0, FailWrite = 0, OpenHandles = 0;
    bool FailOpen = false;
} registry;
NTSTATUS IoOpenDeviceRegistryKey(void *, unsigned, unsigned, HANDLE *key)
{
    if (registry.FailOpen) return STATUS_UNSUCCESSFUL;
    ++registry.OpenHandles; *key = &registry; return STATUS_SUCCESS;
}
void ZwClose(HANDLE key) { check(key == &registry && registry.OpenHandles != 0, "owned registry handle closed"); --registry.OpenHandles; }
NTSTATUS ZwSetValueKey(HANDLE key, UNICODE_STRING *name, unsigned, unsigned, void *data, ULONG length)
{
    check(key == &registry && registry.OpenHandles != 0, "write uses live registry handle");
    if (++registry.Writes == registry.FailWrite) return STATUS_UNSUCCESSFUL;
    auto bytes = static_cast<unsigned char *>(data);
    registry.Values[name->name] = std::vector<unsigned char>(bytes, bytes + length);
    return STATUS_SUCCESS;
}
void KeWaitForSingleObject(unsigned *mutex, unsigned, unsigned, bool, void *)
{ check(*mutex == 0, "diagnostic mutex is not recursively acquired"); *mutex = 1; }
void KeReleaseMutex(unsigned *mutex, bool) { check(*mutex == 1, "diagnostic mutex owned"); *mutex = 0; }
UINT InterlockedCompareExchange(UINT *value, UINT, UINT) { return *value; }
bool ExAcquireRundownProtection(bool *live) { return *live; }
void ExReleaseRundownProtection(bool *live) { check(*live, "hardware reference balanced"); }
bool VioGpuWddmIsRenderOnlyRegistration() { return false; }
struct VioGpuAdapter
{
    bool Retired = false;
    UINT NativeReadinessFailMask() { check(!Retired, "retired hardware is never dereferenced"); return 0x100; }
};
struct DXGK_DRIVERCAPS
{
    UINT WDDMVersion;
    struct { UINT Value; } SchedulingCaps, MemoryManagementCaps;
    struct { UINT NbAsymetricProcessingNodes; } GpuEngineTopology;
    unsigned char PrefixPadding[512];
    struct { UINT GraphicsPreemptionGranularity, ComputePreemptionGranularity; } PreemptionCaps;
    UINT SupportPerEngineTDR;
};
struct DXGK_QUERYSEGMENTOUT4 { UINT NbSegment; void *Poison; };
struct DXGKARG_QUERYADAPTERINFO { UINT Type; void *pInputData; UINT InputDataSize; void *pOutputData; UINT OutputDataSize; };
struct VioGpuDod
{
    unsigned m_NativeActivationTraceMutex = 0;
    VioGpuActivationTrace m_NativeActivationTrace = {};
    void *m_pPhysicalDevice = this;
    bool m_HardwareOperations = true;
    VioGpuAdapter Hardware;
    VioGpuAdapter *m_pHWDevice = &Hardware;
    UINT m_DodReadinessFailMask = 0;
    bool Active = true, Initialized = true, Reset = false;
    bool IsDriverActive() { return Active; }
    bool IsHardwareInit() { return Initialized; }
    bool IsHardwareResetRequested() { return Reset; }
    NTSTATUS ReadRegistryDWORD(HANDLE, const wchar_t *name, DWORD *value)
    {
        auto found = registry.Values.find(name);
        if (found == registry.Values.end()) return STATUS_OBJECT_NAME_NOT_FOUND;
        if (found->second.size() != sizeof(*value)) return STATUS_UNSUCCESSFUL;
        std::memcpy(value, found->second.data(), sizeof(*value)); return STATUS_SUCCESS;
    }
    NTSTATUS WriteRegistryDWORD(HANDLE key, const wchar_t *name, DWORD *value)
    { UNICODE_STRING n{name}; return ZwSetValueKey(key, &n, 0, 4, value, sizeof(*value)); }
    VOID InitializeNativeActivationTrace();
    VOID PersistNativeActivationTrace();
    VOID RecordNativeActivationPhase(VioGpuActivationPhase phase);
    VOID RecordNativeActivationQuery(CONST DXGKARG_QUERYADAPTERINFO *query, NTSTATUS status);
};

// INSERT_PRODUCTION

DWORD regword(const wchar_t *name)
{
    DWORD result = 0;
    auto found = registry.Values.find(name);
    check(found != registry.Values.end() && found->second.size() == 4, "registry DWORD present");
    std::memcpy(&result, found->second.data(), 4); return result;
}
VioGpuActivationTrace blob()
{
    VioGpuActivationTrace result;
    auto &bytes = registry.Values.at(L"NativeActivationTrace");
    check(bytes.size() == sizeof(result), "atomic binary trace exact extent");
    std::memcpy(&result, bytes.data(), sizeof(result)); return result;
}

int main()
{
    VioGpuDod adapter;
    adapter.InitializeNativeActivationTrace();
    check(regword(L"NativeActivationEpoch") == 2 && blob().Epoch == 2 && blob().Version == 2, "first start uses committed even epoch");
    check(blob().Phase == VioGpuActivationStarting && blob().TotalQueries == 0, "fresh trace cleared");
    VioGpuActivationStartStage(&adapter.m_NativeActivationTrace, 0x200, 0xc0000001U, 5);
    VioGpuActivationStartStage(&adapter.m_NativeActivationTrace, 0xfff, 0, 0);
    check(adapter.m_NativeActivationTrace.FirstStartFailureStage == 0x200 && adapter.m_NativeActivationTrace.StartStatus == 0xc0000001U,
          "later transport stage cannot overwrite first startup failure");
    adapter.InitializeNativeActivationTrace();
    check(blob().Epoch == 4 && blob().FirstStartFailureStage == 0, "second start gets a new epoch and clears old failure");
    adapter.RecordNativeActivationPhase(VioGpuActivationActive);
    DXGKARG_QUERYADAPTERINFO query{99, nullptr, 0, nullptr, 0};
    for (unsigned i = 0; i < 66; ++i) adapter.RecordNativeActivationQuery(&query, STATUS_SUCCESS);
    query.pOutputData = reinterpret_cast<void *>(uintptr_t(1));
    query.OutputDataSize = 4096;
    query.Type = DXGKQAITYPE_DRIVERCAPS;
    adapter.RecordNativeActivationQuery(&query, STATUS_DEVICE_NOT_READY);
    auto trace = blob();
    check(trace.Count == 64 && trace.TotalQueries == 67 && trace.FailureCount == 1 && trace.FirstFailure.Sequence == 67,
          "first failure retained after first64 successful queries");
    check(trace.FirstFailure.Values[0] == 0 && trace.FirstFailure.Lifecycle == 27 && trace.FirstFailure.ReadinessMask == 0x100,
          "failed query never reads poisoned output, records live snapshot");
    adapter.RecordNativeActivationPhase(VioGpuActivationStopping);
    adapter.m_HardwareOperations = false;
    adapter.Hardware.Retired = true;
    adapter.Reset = true;
    adapter.RecordNativeActivationQuery(&query, STATUS_UNSUCCESSFUL);
    trace = blob();
    check(trace.FirstFailure.Sequence == 67 && trace.LastFailure.Sequence == 68 && trace.LastFailure.Phase == VioGpuActivationStopping,
          "teardown failure preserves original failure and phase");
    check(trace.LastFailure.Lifecycle == 7 && trace.LastFailure.ReadinessMask == 0, "closed hardware rundown blocks dereference");
    adapter.Hardware.Retired = false;
    adapter.m_HardwareOperations = true;
    adapter.InitializeNativeActivationTrace();
    DXGK_QUERYSEGMENTOUT4 segment{1, reinterpret_cast<void *>(uintptr_t(1))};
    query.Type = DXGKQAITYPE_QUERYSEGMENT4; query.pOutputData = &segment; query.OutputDataSize = sizeof(segment);
    adapter.RecordNativeActivationQuery(&query, STATUS_SUCCESS);
    check(blob().Entries[0].Values[0] == 1 && blob().Entries[0].Values[1] == 0, "count query ignores undefined descriptor tail");
    registry.FailWrite = registry.Writes + 2;
    adapter.RecordNativeActivationQuery(&query, STATUS_SUCCESS);
    check(regword(L"NativeActivationWriteStatus") == UINT(STATUS_UNSUCCESSFUL) && blob().TotalQueries == 1,
          "failed binary write is visible and preserves old complete blob");
    registry.FailWrite = 0;
    adapter.RecordNativeActivationQuery(&query, STATUS_SUCCESS);
    check(regword(L"NativeActivationWriteStatus") == 0 && blob().TotalQueries == 3, "next successful snapshot recovers all in-memory events");
    registry.FailWrite = registry.Writes + 3;
    adapter.RecordNativeActivationQuery(&query, STATUS_SUCCESS);
    check(regword(L"NativeActivationWriteStatus") == UINT(STATUS_PENDING), "failed final marker cannot certify an uncommitted snapshot");
    for (unsigned fault = 1; fault <= 5; ++fault)
    {
        registry = {}; registry.FailWrite = fault;
        adapter.InitializeNativeActivationTrace();
        check(adapter.m_NativeActivationTrace.Version == 0 && registry.OpenHandles == 0 && adapter.m_NativeActivationTraceMutex == 0,
              "every initial registry write failure disables recorder and releases ownership");
        if (fault == 3 || fault == 4) check((regword(L"NativeActivationEpoch") & 1) != 0, "partial start remains invalid odd epoch");
        if (fault == 5) check(regword(L"NativeActivationWriteStatus") == UINT(STATUS_PENDING), "start final marker failure remains invalid pending state");
    }
    registry = {}; registry.FailOpen = true;
    adapter.InitializeNativeActivationTrace();
    check(adapter.m_NativeActivationTrace.Version == 0 && registry.OpenHandles == 0, "registry open failure does not retain old trace");
    UINT epoch = 0;
    check(!VioGpuActivationNextEpoch(0xfffffffeU, &epoch) && !VioGpuActivationNextEpoch(0xffffffffU, &epoch), "epoch wrap never reuses an old identity");
    VioGpuActivationInitialize(&adapter.m_NativeActivationTrace, 2, 0x5023, 0);
    adapter.m_NativeActivationTrace.TotalQueries = 0xffffffffU;
    VioGpuActivationQuery entry = {};
    VioGpuActivationAppend(&adapter.m_NativeActivationTrace, entry);
    check(adapter.m_NativeActivationTrace.CounterOverflow == 1 && adapter.m_NativeActivationTrace.TotalQueries == 0xffffffffU,
          "query overflow explicitly marked without sequence reuse");
    std::printf("PASS: %u production activation recorder/lifetime/registry assertions\n", checks);
}
