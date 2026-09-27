// The production startup block, ISR and worker creation are inserted by run.py.
// Peers model PCI INTx status coalescing and Dxgk DPC dispatch, not Windows timing.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <utility>

using BOOLEAN = bool;
using LONG = int32_t;
using ULONG = int32_t;
using PLONG = LONG*;
using UCHAR = uint8_t;
using NTSTATUS = int;
using VOID = void;
using HANDLE = void*;
using PVOID = void*;
using PETHREAD = void*;
constexpr bool TRUE = true, FALSE = false;
constexpr int STATUS_SUCCESS = 0, STATUS_DEVICE_NOT_READY = -1;
constexpr int SYNCHRONIZE = 0, KernelMode = 0, IO_NO_INCREMENT = 0;
constexpr ULONG ISR_REASON_CHANGE = 1, ISR_REASON_DISPLAY = 2, ISR_REASON_CURSOR = 4;
constexpr UCHAR VIRTIO_PCI_ISR_CONFIG = 2, VIRTIO_CONFIG_S_DRIVER_OK = 4;
constexpr int DXGK_INTERRUPT_DISPLAYONLY_PRESENT_PROGRESS = 0;
constexpr int DXGK_PRESENT_DISPLAYONLY_PROGRESS_ID_COMPLETE = 0;
#define _In_
#define NT_SUCCESS(s) ((s) >= 0)
#define PAGED_CODE() ((void)0)
#define DbgPrint(...) ((void)0)
#define DbgPrintEx(...) ((void)0)
#define VIOGPU_RECORD_NATIVE_START(...) ((void)0)
#define UNREFERENCED_PARAMETER(x) ((void)(x))

static void require(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(2); }
}
static LONG InterlockedCompareExchange(LONG* value, LONG desired, LONG expected)
{ LONG old = *value; if (old == expected) *value = desired; return old; }
static LONG InterlockedExchange(LONG* value, LONG desired)
{ return std::exchange(*value, desired); }
static int64_t InterlockedCompareExchange64(int64_t* value, int64_t desired, int64_t expected)
{ auto old = *value; if (old == expected) *value = desired; return old; }
static void InterlockedOr(PLONG value, ULONG mask) { *value |= mask; }
struct Counter { int64_t QuadPart; };
static Counter KeQueryPerformanceCounter(void*) { return {1}; }
static void KeClearEvent(bool* event) { *event = false; }
static void KeSetEvent(bool* event, int, bool) { *event = true; }

struct Queue {
    bool interrupts = false, enableOk = true, healthy = false;
    bool EnableInterrupt() { interrupts = enableOk; return interrupts; }
    bool EnableNativeSynchronousRequests() { return true; }
    bool EnableSynchronousRequests() { healthy = true; return true; }
    bool IsSynchronousRequestsHealthy() { return healthy; }
};
struct Dod {
    bool initialized = false;
    void SetHardwareInit(bool value) { initialized = value; }
    bool IsRenderOnly() { return true; }
    bool IsUsePresentProgress() { return false; }
};
struct Pci {
    bool msi = false;
    bool IsMSIEnabled() { return msi; }
};
struct VioGpuAdapter;
struct VirtioDevice {
    VioGpuAdapter* owner;
    UCHAR isr = 0, status = 0;
};
struct DXGKARGCB_NOTIFY_INTERRUPT_DATA {
    int InterruptType;
    struct { int VidPnSourceId, ProgressId; } DisplayOnlyPresentProgress;
};
struct DXGKRNL_INTERFACE {
    void* DeviceHandle;
    void (*DxgkCbQueueDpc)(void*);
    void (*DxgkCbNotifyInterrupt)(void*, DXGKARGCB_NOTIFY_INTERRUPT_DATA*);
};
using PDXGKRNL_INTERFACE = DXGKRNL_INTERFACE*;
struct VioGpuAdapter {
    Dod dod;
    Dod* m_pVioGpuDod = &dod;
    Queue m_CtrlQueue, m_CursorQueue;
    Pci m_PciResources;
    VirtioDevice m_VioDev{this};
    LONG m_InterruptDispatchEnabled = FALSE;
    ULONG m_PendingWorks = 0;
    int64_t m_DisplayIsrTicks = 0;
    bool m_ConfigUpdateEvent = false, m_bStopWorkThread = false;
    HANDLE m_WorkThreadHandle = nullptr;
    PETHREAD m_pWorkThread = nullptr;
    bool inlineConfig = false, deferDpc = false, rejectReady = false;
    int readyWrites = 0, controlCompletions = 0;
    NTSTATUS InitializeControlTransport();
    BOOLEAN InterruptRoutine(PDXGKRNL_INTERFACE, ULONG);
    NTSTATUS StartWorkThread();
    static void ThreadWork(PVOID) {}
};
static void dispatchDpc(void* context)
{
    auto& adapter = *static_cast<VioGpuAdapter*>(context);
    if (adapter.deferDpc) return;
    // Same notification side effects as VioGpuAdapter::DpcRoutine.
    auto pending = std::exchange(adapter.m_PendingWorks, 0);
    if (pending & ISR_REASON_CHANGE) KeSetEvent(&adapter.m_ConfigUpdateEvent, 0, false);
    if (pending & ISR_REASON_DISPLAY) ++adapter.controlCompletions;
}
static void notifyInterrupt(void*, DXGKARGCB_NOTIFY_INTERRUPT_DATA*) {}
static void signal(VioGpuAdapter& adapter, UCHAR mask)
{
    auto old = adapter.m_VioDev.isr;
    if (!adapter.m_PciResources.msi) adapter.m_VioDev.isr |= mask;
    if (adapter.m_PciResources.msi || old == 0) {
        DXGKRNL_INTERFACE interface{&adapter, dispatchDpc, notifyInterrupt};
        adapter.InterruptRoutine(&interface, mask == VIRTIO_PCI_ISR_CONFIG ? 0 : 1);
    }
}
static UCHAR virtio_read_isr_status(VirtioDevice* device)
{ return std::exchange(device->isr, 0); }
static UCHAR virtio_get_status(VirtioDevice* device) { return device->status; }
static void virtio_device_ready(VirtioDevice* device)
{
    auto& adapter = *device->owner;
    require(adapter.m_CtrlQueue.interrupts && adapter.m_CursorQueue.interrupts,
            "DRIVER_OK published before both queues were initialized");
    ++adapter.readyWrites;
    device->status = adapter.rejectReady ? 0 : VIRTIO_CONFIG_S_DRIVER_OK;
    if (adapter.inlineConfig) signal(adapter, VIRTIO_PCI_ISR_CONFIG);
}
static int threadObject;
static void* threadType;
static void** PsThreadType = &threadType;
static NTSTATUS PsCreateSystemThread(HANDLE* handle, int, void*, HANDLE, void*, void (*)(PVOID), void*)
{ *handle = &threadObject; return STATUS_SUCCESS; }
static NTSTATUS ObReferenceObjectByHandle(HANDLE handle, int, void*, int, PVOID* object, void*)
{ *object = handle; return STATUS_SUCCESS; }
static void ZwClose(HANDLE) {}

// INSERT_PRODUCTION

int main()
{
    int scenarios = 0;
    for (bool msi : {false, true}) {
        for (bool inlineConfig : {false, true}) {
            for (bool deferred : {false, true}) {
                VioGpuAdapter adapter;
                adapter.m_PciResources.msi = msi;
                adapter.inlineConfig = inlineConfig;
                adapter.deferDpc = deferred;
                // A previous activation's event must not survive reset.
                adapter.m_ConfigUpdateEvent = true;
                require(adapter.InitializeControlTransport() == STATUS_SUCCESS, "startup failed");
                // Complete GET_CAPSET_INFO after the MMIO DRIVER_OK write.
                signal(adapter, 1);
                adapter.deferDpc = false;
                dispatchDpc(&adapter);
                require(adapter.controlCompletions == 1, "first capset response interrupt lost");
                require(adapter.StartWorkThread() == STATUS_SUCCESS, "worker startup failed");
                require(adapter.m_ConfigUpdateEvent == inlineConfig,
                        "activation profile event lost or stale event retained");
                // An independent later attachment must still notify the worker.
                adapter.m_ConfigUpdateEvent = false;
                signal(adapter, VIRTIO_PCI_ISR_CONFIG);
                require(adapter.m_ConfigUpdateEvent, "post-start profile event lost");
                ++scenarios;
            }
        }
    }
    for (bool controlFailure : {false, true}) {
        VioGpuAdapter adapter;
        (controlFailure ? adapter.m_CtrlQueue : adapter.m_CursorQueue).enableOk = false;
        require(adapter.InitializeControlTransport() == STATUS_DEVICE_NOT_READY, "queue failure ignored");
        require(adapter.readyWrites == 0 && !adapter.m_InterruptDispatchEnabled,
                "failed queue published ready interrupts");
        ++scenarios;
    }
    VioGpuAdapter rejected;
    rejected.rejectReady = true;
    require(rejected.InitializeControlTransport() == STATUS_DEVICE_NOT_READY, "rejected DRIVER_OK accepted");
    require(!rejected.dod.initialized, "rejected DRIVER_OK published hardware initialized");
    std::printf("PASS startup/ISR/worker: %d interleaving and failure scenarios\n", scenarios + 1);
}
