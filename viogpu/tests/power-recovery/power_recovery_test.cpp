#include <cstdint>
#include <cstdio>
#include <vector>

using LONG = int32_t;
using ULONG = uint32_t;
using NTSTATUS = int32_t;
using BOOLEAN = bool;
enum DEVICE_POWER_STATE { PowerDeviceUnspecified, PowerDeviceD0, PowerDeviceD1, PowerDeviceD2, PowerDeviceD3 };
enum POWER_ACTION { PowerActionNone };
enum { VioGpuHardwareActive, VioGpuHardwareResetRequested, VioGpuHardwareRecovering };
constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS STATUS_DEVICE_NOT_READY = static_cast<int32_t>(0xc00000a3u);
constexpr NTSTATUS TeardownFailure = -17;
constexpr ULONG DISPLAY_ADAPTER_HW_ID = 0;
constexpr ULONG D3DDDI_ID_ALL = ~0u;
#ifndef _In_
#define _In_
#endif
#define TRUE true
#define FALSE false
#define VIOGPU_NATIVE_CONTEXT 1
#define PAGED_CODE() ((void)0)
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define DbgPrint(...) ((void)0)
#define VioGpuDbgBreak() ((void)0)
#define NT_SUCCESS(x) ((x) >= 0)
LONG InterlockedCompareExchange(LONG* target, LONG value, LONG expected)
{
    LONG old = *target;
    if (old == expected) *target = value;
    return old;
}
LONG InterlockedExchange(LONG* target, LONG value)
{
    LONG old = *target;
    *target = value;
    return old;
}
struct DXGKARG_SETVIDPNSOURCEVISIBILITY { ULONG VidPnSourceId; BOOLEAN Visible; };
struct DisplayInfo { unsigned Width = 1920; };
struct VioGpuDod;
struct Adapter {
    VioGpuDod* owner;
    explicit Adapter(VioGpuDod* device) : owner(device) {}
    NTSTATUS closeStatus = STATUS_SUCCESS;
    NTSTATUS startStatus = STATUS_SUCCESS;
    bool resetDuringClose = false, resetDuringStart = false;
    bool interruptsAtStart = false;
    unsigned faultNotifications = 0;
    std::vector<DEVICE_POWER_STATE> transitions;
    void ResetDevice();
    NTSTATUS SetPowerState(int*, DEVICE_POWER_STATE, int*);
};
struct VioGpuDod {
    mutable LONG m_HardwareResetState = VioGpuHardwareResetRequested;
    LONG m_HardwareResetCallerRva = 1, m_HardwareResetFirstCallerRva = 1;
    DEVICE_POWER_STATE m_AdapterPowerState = PowerDeviceD3;
    DisplayInfo m_SystemDisplayInfo;
    int m_DeviceInfo = 0, m_CurrentMode = 0;
    Adapter adapter{this};
    Adapter* m_pHWDevice = &adapter;
    struct Interface {
        NTSTATUS (*DxgkCbAcquirePostDisplayOwnership)(void*, DisplayInfo*) = nullptr;
        void* DeviceHandle = nullptr;
    } m_DxgkInterface;
    bool drainSucceeds = true, idleSucceeds = true, openSucceeds = true;
    bool drained = false, opened = false;
    unsigned fences = 0;
    void RequestWddmSubmissionDrainAtAnyIrql() { drained = true; opened = false; }
    bool WaitForWddmSubmissionDrain() { return drainSucceeds; }
    bool WaitForNativePassiveQueueIdle() { return idleSucceeds; }
    bool OpenNativePassiveQueue() { return openSucceeds; }
    bool OpenWddmPresentTransactions() { opened = openSucceeds; return openSucceeds; }
    void CompleteNativeFenceReset() { ++fences; }
    void RequestHardwareResetAtAnyIrql() { m_HardwareResetState = VioGpuHardwareResetRequested; opened = false; }
    void SetVidPnSourceVisibility(DXGKARG_SETVIDPNSOURCEVISIBILITY*) {}
    NTSTATUS SetPowerState(ULONG, DEVICE_POWER_STATE, POWER_ACTION, BOOLEAN);
    // INSERT_INTERRUPT_PREDICATE
};
void Adapter::ResetDevice() { ++faultNotifications; owner->RequestHardwareResetAtAnyIrql(); }
NTSTATUS Adapter::SetPowerState(int*, DEVICE_POWER_STATE state, int*)
{
    transitions.push_back(state);
    if (state == PowerDeviceD3) {
        if (resetDuringClose) owner->RequestHardwareResetAtAnyIrql();
        return closeStatus;
    }
    interruptsAtStart = owner->IsHardwareInterruptDispatchAllowed();
    if (resetDuringStart) owner->RequestHardwareResetAtAnyIrql();
    return interruptsAtStart ? startStatus : STATUS_DEVICE_NOT_READY;
}
// INSERT_POWER_FUNCTION

int main()
{
    unsigned failures = 0, checks = 0;
    auto check = [&](bool condition, const char* name) {
        ++checks;
        if (!condition) { ++failures; std::printf("FAIL: %s\n", name); }
    };
    auto restart = [](VioGpuDod& device) {
        return device.SetPowerState(DISPLAY_ADAPTER_HW_ID, PowerDeviceD0, PowerActionNone, FALSE);
    };
    for (auto previousPower : {PowerDeviceD0, PowerDeviceD3}) {
        VioGpuDod device;
        device.m_AdapterPowerState = previousPower;
        check(restart(device) == STATUS_SUCCESS, "D0 recovery completes");
        check(device.adapter.transitions == std::vector<DEVICE_POWER_STATE>{PowerDeviceD3, PowerDeviceD0},
              "old transport retired before new start, including already-D0 faults");
        check(device.adapter.interruptsAtStart && device.adapter.faultNotifications == 0,
              "recovery services interrupts without self-requesting another reset");
        check(device.drained && device.opened && device.fences == 1 &&
              device.m_HardwareResetState == VioGpuHardwareActive, "publish only completed recovery");
    }
    for (unsigned failure = 0; failure < 7; ++failure) {
        VioGpuDod device;
        if (failure == 0) device.drainSucceeds = false;
        if (failure == 1) device.adapter.closeStatus = TeardownFailure;
        if (failure == 2) device.adapter.startStatus = STATUS_DEVICE_NOT_READY;
        if (failure == 3) device.adapter.resetDuringClose = true;
        if (failure == 4) device.adapter.resetDuringStart = true;
        if (failure == 5) device.idleSucceeds = false;
        if (failure == 6) device.openSucceeds = false;
        NTSTATUS status = restart(device);
        check(status == (failure == 1 ? TeardownFailure : STATUS_DEVICE_NOT_READY), "propagate recovery failure");
        check(!device.opened && device.m_HardwareResetState == VioGpuHardwareResetRequested,
              "drain/teardown/start/concurrent reset/idle/reopen failures stay closed");
        if (failure < 2) check(!device.adapter.interruptsAtStart, "failed teardown cannot start new transport");
    }
    VioGpuDod active;
    active.m_HardwareResetState = VioGpuHardwareActive;
    active.m_AdapterPowerState = PowerDeviceD0;
    check(restart(active) == STATUS_SUCCESS && active.fences == 0 &&
          active.adapter.transitions == std::vector<DEVICE_POWER_STATE>{PowerDeviceD0}, "ordinary active D0 is preserved");
    VioGpuDod competing;
    competing.m_HardwareResetState = VioGpuHardwareRecovering;
    check(restart(competing) == STATUS_DEVICE_NOT_READY && competing.adapter.transitions.empty(),
          "competing recovery cannot claim transport");
    VioGpuDod post;
    post.m_DxgkInterface.DxgkCbAcquirePostDisplayOwnership = [](void*, DisplayInfo*) { return TeardownFailure; };
    check(post.SetPowerState(0, PowerDeviceD0, PowerActionNone, TRUE) == TeardownFailure &&
          post.adapter.transitions.empty(), "ordinary OS D0 still honors POST failure");
    check(restart(post) == STATUS_SUCCESS, "TDR bypasses POST handoff");
    std::printf("Production power recovery: %u/%u checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
