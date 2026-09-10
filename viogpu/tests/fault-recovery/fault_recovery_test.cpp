#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

using UINT = uint32_t;
using ULONG = uint32_t;
using DWORD = uint32_t;
using LONG = int32_t;
using ULONG_PTR = uintptr_t;
using NTSTATUS = int32_t;
using BOOLEAN = bool;
using KIRQL = unsigned;
using HANDLE = void*;
#ifndef _In_
#define _In_
#endif
#ifndef _Use_decl_annotations_
#define _Use_decl_annotations_
#endif
#define APIENTRY
#define CONST const
#ifndef _MSC_VER
#define __declspec(x)
#endif
#define _ReturnAddress() nullptr
#define __ImageBase simulatedImageBase
#define VIOGPU_NATIVE_CONTEXT 1
#define TRUE true
#define FALSE false
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define NT_SUCCESS(x) ((x) >= 0)
#define RtlZeroMemory(p, n) std::memset(p, 0, n)
#define KeMemoryBarrier() ((void)0)
#define MAXULONG UINT32_MAX
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_INVALID_PARAMETER = -1;
constexpr unsigned DXGK_INTERRUPT_DMA_FAULTED = 4, DXGK_INTERRUPT_DMA_PREEMPTED = 2;
char __ImageBase;
LONG InterlockedExchange(LONG* p, LONG v) { LONG old = *p; *p = v; return old; }
LONG InterlockedIncrement(LONG* p) { return ++*p; }
LONG InterlockedCompareExchange(LONG* p, LONG v, LONG old)
{ LONG result = *p; if (result == old) *p = v; return result; }
void KeAcquireSpinLock(unsigned*, KIRQL* irql) { *irql = 0; }
void KeReleaseSpinLock(unsigned*, KIRQL) {}
bool ExAcquireRundownProtection(bool* available) { return *available; }
void ExReleaseRundownProtection(bool*) {}
struct DXGKARGCB_NOTIFY_INTERRUPT_DATA {
    unsigned InterruptType;
    struct { UINT FaultedFenceId; NTSTATUS Status; UINT NodeOrdinal, EngineOrdinal; } DmaFaulted;
    struct { UINT PreemptionFenceId, LastCompletedFenceId, NodeOrdinal, EngineOrdinal; } DmaPreempted;
};
struct DXGKARG_PREEMPTCOMMAND {
    UINT PreemptionFenceId = 50, NodeOrdinal = 0, EngineOrdinal = 0;
    struct { unsigned Value = 0; } Flags;
};
struct DXGKARG_QUERYCURRENTFENCE { UINT NodeOrdinal = 0, EngineOrdinal = 0, CurrentFence = 0; };
struct VioGpuAdapter {
    bool failed = false;
    void FailNativeContextAtAnyIrql() { failed = true; }
};
struct VioGpuDod {
    VioGpuAdapter hardware;
    VioGpuAdapter* m_pHWDevice = &hardware;
    bool m_HardwareOperations = true, reset = false;
    LONG m_NativeSubmissionFaultDiagnosticRecorded = 0, m_NativePresentExecutionDiagnosticRecorded = 0;
    LONG m_NativeSubmissionFaultCallerRva = 0, m_NativeSubmissionFaultExecutionDiagnosticState = 0;
    LONG m_NativeSubmissionFaultPresentSubmitStage = 0, m_NativeSubmissionFaultPresentSubmitStatus = 0;
    LONG m_NativeSubmissionFaultPresentSubmitDetail = 0, m_NativeFenceNotificationClosed = 0;
    LONG m_NativeFenceEpoch = 1, m_NativeSubmittedFence = 42, m_NativeCompletedFence = 41;
    LONG m_NativeFenceResetFloor = 0;
    unsigned m_NativeFenceLock = 0, m_NativeFenceHead = 0, m_NativeFenceCount = 1;
    unsigned m_NativeFences[2] = {42, 0};
    bool pendingPreempt = true;
    std::vector<unsigned> notifications;
    void RequestHardwareResetAtAnyIrql() { reset = true; }
    bool IsHardwareResetRequested() const { return reset; }
    ULONG QueryNativeFenceEpoch() const { return static_cast<ULONG>(m_NativeFenceEpoch); }
    UINT QueryNativeCompletedFence() const { return static_cast<UINT>(m_NativeCompletedFence); }
    bool IsNativeFenceQueueEmpty() const { return m_NativeFenceCount == 0; }
    void CountNativePreemptReset() {}
    void ResetDevice() { reset = true; }
    bool DeferNativePreemption(UINT, ULONG) { pendingPreempt = true; return true; }
    void DiscardDeferredNativePreemption() { pendingPreempt = false; }
    bool NotifyNativeSchedulerInterrupt(DXGKARGCB_NOTIFY_INTERRUPT_DATA* data, BOOLEAN, ULONG = 0)
    { notifications.push_back(data->InterruptType); return true; }
    void InvalidateNativeFenceTracker();
    void CompleteNativeFenceReset();
    void NotifyNativeSubmissionFault(UINT, NTSTATUS, UINT, UINT, BOOLEAN, DWORD = 0, NTSTATUS = 0, DWORD = 0);
};
// INSERT_PRODUCTION

int main()
{
    unsigned checks = 0, failures = 0;
    auto check = [&](bool ok, const char* label) {
        ++checks;
        if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
    };
    for (unsigned mode = 0; mode < 4; ++mode) {
        VioGpuDod device;
        if (mode == 2) device.m_HardwareOperations = false;
        if (mode == 3) device.m_pHWDevice = nullptr;
        device.NotifyNativeSubmissionFault(mode == 1 ? 0 : 42, -7, mode == 1 ? 9 : 0, 0, TRUE);
        check(device.reset && device.m_NativeFenceNotificationClosed, "fault closes publication even without identity/transport");
        check(mode >= 2 || device.hardware.failed, "available host transport is invalidated");
        check(device.notifications.empty(), "fault emits no reserved DMA fault or success interrupt");
        DXGKARG_QUERYCURRENTFENCE query;
        check(VioGpuWddmQueryCurrentFence(&device, &query) == STATUS_SUCCESS && query.CurrentFence == 41,
              "scheduler still sees failed packet incomplete");
        DXGKARG_PREEMPTCOMMAND preempt;
        check(VioGpuWddmPreemptCommand(&device, &preempt) == STATUS_SUCCESS && device.notifications.empty(),
              "failed engine cannot acknowledge preemption despite cleared private tracker");
        device.NotifyNativeSubmissionFault(43, -7, 0, 0, FALSE);
        check(device.m_NativeSubmittedFence == 42 && device.m_NativeCompletedFence == 41 && device.notifications.empty(),
              "repeated faults cannot retire scheduler packets or move endpoints");
        // Model only the successful hardware-reset barrier. OS TDR dispatch is
        // deliberately not simulated; it must be checked on the real guest.
        device.CompleteNativeFenceReset();
        check(device.m_NativeCompletedFence == 42 && device.m_NativeFenceResetFloor == 42 && !device.pendingPreempt,
              "successful reset retires abandoned endpoint and old preemption");
    }
    VioGpuDod healthy;
    healthy.m_NativeFenceCount = 0;
    DXGKARG_PREEMPTCOMMAND preempt;
    check(VioGpuWddmPreemptCommand(&healthy, &preempt) == STATUS_SUCCESS &&
          healthy.notifications == std::vector<unsigned>{DXGK_INTERRUPT_DMA_PREEMPTED},
          "healthy idle engine still acknowledges preemption");
    std::printf("Production fault recovery: %u/%u checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
