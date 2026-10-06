#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _MSC_VER
#include <intrin.h>
#else
#define __declspec(value)
#endif
#define _In_
#define _Out_
#define _Inout_
#define VIOGPU_NATIVE_CONTEXT 1
#define PAGED_CODE() ((void)0)
#define UNREFERENCED_PARAMETER(value) ((void)(value))
#define RtlZeroMemory(dest, size) std::memset(dest, 0, size)
using ULONG = uint32_t;
using UINT = uint32_t;
using LONG = int32_t;
using LONG64 = int64_t;
using ULONGLONG = uint64_t;
using ULONG_PTR = uintptr_t;
using BOOLEAN = bool;
using VOID = void;
constexpr bool TRUE = true, FALSE = false;
constexpr ULONG MAXULONG = UINT32_MAX;
constexpr UINT VIOGPU_NATIVE_RESOURCE_ID_START = 0x80000000U;
constexpr int PASSIVE_LEVEL = 0, VioGpuHardwareResetRequested = 1;
enum { VioGpuNativeFailSiteDestroy2DPreexisting, VioGpuNativeFailSiteDestroy2DGeneration, VioGpuNativeFailSiteDestroy2DUnref };
static int checks;
static void check(bool value, const char *message)
{
    ++checks;
    if (!value) { std::printf("FAIL %s\n", message); std::exit(1); }
}
static LONG InterlockedCompareExchange(volatile LONG *dest, LONG value, LONG expected)
{
#ifdef _MSC_VER
    return static_cast<LONG>(_InterlockedCompareExchange(reinterpret_cast<volatile long *>(dest), value, expected));
#else
    __atomic_compare_exchange_n(dest, &expected, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
#endif
}
static LONG InterlockedExchange(volatile LONG *dest, LONG value)
{
#ifdef _MSC_VER
    return static_cast<LONG>(_InterlockedExchange(reinterpret_cast<volatile long *>(dest), value));
#else
    return __atomic_exchange_n(dest, value, __ATOMIC_SEQ_CST);
#endif
}
static LONG64 InterlockedCompareExchange64(volatile LONG64 *dest, LONG64 value, LONG64 expected)
{
#ifdef _MSC_VER
    return _InterlockedCompareExchange64(dest, value, expected);
#else
    __atomic_compare_exchange_n(dest, &expected, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
#endif
}
static ULONGLONG clockTicks = 0x123456789abcdef0ULL;
static ULONGLONG KeQueryInterruptTime() { return clockTicks; }
static int KeGetCurrentIrql() { return PASSIVE_LEVEL; }
static unsigned char __ImageBase;
#define _ReturnAddress() reinterpret_cast<void *>(reinterpret_cast<ULONG_PTR>(&__ImageBase) + 0x1234)
// INSERT_DEFINITIONS
struct VioGpuAdapter;
struct VioGpuDod
{
    volatile LONG m_ResetRequestPublication = 0, m_2DDestroyPublication = 0;
    volatile LONG m_HardwareResetState = 0, m_HardwareResetCallerRva = 0, m_HardwareResetFirstCallerRva = 0;
    VIOGPU_RESET_REQUEST_DIAGNOSTIC m_FirstResetRequest = {};
    VIOGPU_2D_DESTROY_DIAGNOSTIC m_First2DDestroyFailure = {};
    VioGpuAdapter *m_pHWDevice = nullptr;
    bool admission = true;
    unsigned releases = 0, drains = 0;
    ULONG ReadHardwareResetState() { return static_cast<ULONG>(InterlockedCompareExchange(&m_HardwareResetState, 0, 0)); }
    void RequestWddmSubmissionDrainAtAnyIrql() { ++drains; }
    bool AcquireNativeSubmissionOperation() { return admission; }
    void ReleaseNativeSubmissionOperation() { ++releases; }
    __declspec(code_seg(".text")) void RecordFirstResetRequest(ULONG_PTR);
    bool GetFirstResetRequest(VIOGPU_RESET_REQUEST_DIAGNOSTIC *);
    void RecordNative2DDestroyFailure(ULONG, UINT, ULONG, ULONGLONG, ULONGLONG, VIOGPU_HOST_CONTEXT_RESULT);
    bool GetFirst2DDestroyFailure(VIOGPU_2D_DESTROY_DIAGNOSTIC *);
    void RequestHardwareResetAtAnyIrql();
    VIOGPU_HOST_CONTEXT_RESULT Destroy2DResource(UINT, VIOGPU_2D_RESOURCE_STATE *, ULONGLONG *, BOOLEAN *, BOOLEAN);
};
struct VioGpuAdapter
{
    VioGpuDod *m_pVioGpuDod = nullptr;
    volatile LONG64 m_NativeContextResetGeneration = 1, m_2DRetiredResetGeneration = 0;
    struct Queue
    {
        VIOGPU_HOST_CONTEXT_RESULT unmap = VioGpuHostContextConfirmed, unref = VioGpuHostContextConfirmed;
        unsigned unmaps = 0, unrefs = 0;
        VIOGPU_HOST_CONTEXT_RESULT UnmapBlobSynchronous(UINT) { ++unmaps; return unmap; }
        VIOGPU_HOST_CONTEXT_RESULT UnrefResourceSynchronous(UINT) { ++unrefs; return unref; }
    } m_CtrlQueue;
    unsigned failures = 0;
    void FailNativeContextAtAnyIrql(int)
    {
        ++failures;
        if (m_pVioGpuDod) m_pVioGpuDod->RequestHardwareResetAtAnyIrql();
    }
    VIOGPU_HOST_CONTEXT_RESULT Destroy2DResource(UINT, VIOGPU_2D_RESOURCE_STATE *, ULONGLONG *, BOOLEAN *, BOOLEAN);
    bool Reconcile2DResourceAfterReset(VIOGPU_2D_RESOURCE_STATE *, ULONGLONG *, BOOLEAN *);
};
// INSERT_PRODUCTION

static void resetCapture()
{
    VioGpuDod dod;
    VIOGPU_RESET_REQUEST_DIAGNOSTIC diagnostic = {};
    check(!dod.GetFirstResetRequest(&diagnostic) && !dod.GetFirstResetRequest(nullptr), "empty and null reset readers refused");
    dod.RequestHardwareResetAtAnyIrql();
    check(dod.GetFirstResetRequest(&diagnostic) && diagnostic.HardwareState == 0 &&
              dod.m_HardwareResetState == 1 && dod.drains == 1, "first reset observation precedes the reset latch");
    check(diagnostic.CallerRva == 0x1234 && diagnostic.TimeLow == 0x9abcdef0 && diagnostic.TimeHigh == 0x12345678,
          "reset caller and both clock halves captured");
    const auto first = diagnostic;
    dod.m_HardwareResetState = 0;
    dod.m_HardwareResetFirstCallerRva = 0;
    clockTicks += 1000;
    dod.RequestHardwareResetAtAnyIrql();
    check(dod.GetFirstResetRequest(&diagnostic) && !std::memcmp(&first, &diagnostic, sizeof(first)) && dod.drains == 2,
          "first reset request is immutable");
    dod.m_ResetRequestPublication = 1;
    diagnostic.CallerRva = 99;
    check(!dod.GetFirstResetRequest(&diagnostic) && !diagnostic.CallerRva, "partial reset publication hidden");
}

static void destroyCapture()
{
    VioGpuDod dod;
    VioGpuAdapter adapter;
    dod.m_pHWDevice = &adapter;
    adapter.m_pVioGpuDod = &dod;
    VIOGPU_2D_DESTROY_DIAGNOSTIC diagnostic = {};
    check(!dod.GetFirst2DDestroyFailure(&diagnostic) && !dod.GetFirst2DDestroyFailure(nullptr), "empty and null destroy readers refused");
    for (ULONG stage = VioGpu2DDestroyArguments; stage <= VioGpu2DDestroyUnref; ++stage)
    {
        dod.m_2DDestroyPublication = 0;
        dod.admission = stage != VioGpu2DDestroyRundown;
        dod.m_pHWDevice = stage == VioGpu2DDestroyAdapter ? nullptr : &adapter;
        adapter.m_CtrlQueue = {};
        adapter.failures = 0;
        if (stage == VioGpu2DDestroyUnmap) adapter.m_CtrlQueue.unmap = VioGpuHostContextNotSubmitted;
        if (stage == VioGpu2DDestroyUnref) adapter.m_CtrlQueue.unref = VioGpuHostContextNotSubmitted;
        VIOGPU_2D_RESOURCE_STATE state = stage == VioGpu2DDestroyLedger ? VioGpu2DResourceUnknown : VioGpu2DResourceNativeAhbMapped;
        ULONGLONG generation = stage == VioGpu2DDestroyReconcile ? 0 : 1;
        BOOLEAN released = true;
        const auto result = dod.Destroy2DResource(17, &state, &generation,
                                                 stage == VioGpu2DDestroyArguments ? nullptr : &released, false);
        check(result != VioGpuHostContextConfirmed && dod.GetFirst2DDestroyFailure(&diagnostic), "each destroy refusal captured");
        if (stage == VioGpu2DDestroyUnmap)
            check(diagnostic.Stage == stage && adapter.m_CtrlQueue.unmaps == 1 && !adapter.m_CtrlQueue.unrefs &&
                      state == VioGpu2DResourceNativeAhbMapped && generation == 1 && !released,
                  "unmap failure is distinct and does not submit UNREF");
        else
            check(diagnostic.Stage == stage && diagnostic.ResourceId == 17 && diagnostic.Result == static_cast<ULONG>(result),
                  "destroy gate and result remain distinct");
        if (stage == VioGpu2DDestroyUnref)
            check(adapter.m_CtrlQueue.unmaps == 1 && adapter.m_CtrlQueue.unrefs == 1 && !adapter.failures &&
                      diagnostic.ResourceState == VioGpu2DResourceNativeAhbMapped &&
                      diagnostic.ResourceGenerationLow == 1 && diagnostic.AdapterGenerationLow == 1,
                  "UNREF admission failure retains ledger and does not introduce escalation");
    }
    const auto first = diagnostic;
    dod.RecordNative2DDestroyFailure(123, 456, 789, 0x100000002ULL, 0x300000004ULL, VioGpuHostContextUnknown);
    check(dod.GetFirst2DDestroyFailure(&diagnostic) && !std::memcmp(&first, &diagnostic, sizeof(first)),
          "first destroy refusal is immutable");
    dod.m_2DDestroyPublication = 1;
    diagnostic.Stage = 99;
    check(!dod.GetFirst2DDestroyFailure(&diagnostic) && !diagnostic.Stage, "partial destroy publication hidden");
    dod.m_2DDestroyPublication = 0;
    adapter.m_CtrlQueue = {};
    VIOGPU_2D_RESOURCE_STATE state = VioGpu2DResourceNativeAhbMapped;
    ULONGLONG generation = 1;
    BOOLEAN released = false;
    check(dod.Destroy2DResource(17, &state, &generation, &released, false) == VioGpuHostContextConfirmed && released &&
              state == VioGpu2DResourceNone && generation == 0 && !dod.GetFirst2DDestroyFailure(&diagnostic) &&
              adapter.m_CtrlQueue.unmaps == 1 && adapter.m_CtrlQueue.unrefs == 1,
          "successful destruction and transport order unchanged");
}
int main()
{
    resetCapture();
    destroyCapture();
    std::printf("PASS readiness diagnostic: %d checks\n", checks);
}
