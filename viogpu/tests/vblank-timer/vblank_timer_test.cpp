// Production callback/arm/due/rearm/deliver/disarm bodies, with an EX_TIMER peer that
// disables new operations and waits for callbacks before deleting the object.
// This exercises driver ordering; it is not a Windows kernel execution test.
#include "display_timing.h"
#include "vblank_cadence.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

using LONG = int32_t;
using ULONG = uint32_t;
using UINT = unsigned;
using ULONGLONG = unsigned long long;
using LONG64 = long long;
using LONGLONG = long long;
using VOID = void;
using PVOID = void*;
using HANDLE = void*;
using BOOLEAN = bool;
using NTSTATUS = int32_t;
using KIRQL = int;
using FAST_MUTEX = std::mutex;
using KSPIN_LOCK = std::mutex;
struct LARGE_INTEGER { LONGLONG QuadPart; };
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_INVALID_PARAMETER = -1, STATUS_INSUFFICIENT_RESOURCES = -2;
constexpr NTSTATUS STATUS_DEVICE_NOT_READY = -3;
constexpr int REG_BINARY = 3;
struct UNICODE_STRING { const wchar_t* Buffer; };
void RtlInitUnicodeString(UNICODE_STRING* name, const wchar_t* text) { name->Buffer = text; }
static VIOGPU_VBLANK_CADENCE_SNAPSHOT publishedCadence{};
static unsigned cadencePublications;
static std::function<void()> insidePublish;
static NTSTATUS publicationStatus = STATUS_SUCCESS;
NTSTATUS ZwSetValueKey(HANDLE, UNICODE_STRING* name, unsigned, unsigned kind, void* value, unsigned size) {
    if (insidePublish) insidePublish();
    if (std::wcscmp(name->Buffer, L"NativeVblankCadenceSnapshot") ||
        kind != REG_BINARY || size != sizeof(publishedCadence)) return STATUS_INVALID_PARAMETER;
    if (publicationStatus != STATUS_SUCCESS) return publicationStatus;
    std::memcpy(&publishedCadence, value, size);
    ++cadencePublications;
    return STATUS_SUCCESS;
}
constexpr int EX_TIMER_HIGH_RESOLUTION = 1, VioGpuVsyncLateOver1ms = 1;
#define _In_
#define _In_opt_
#define _Inout_
#define TRUE true
#define FALSE false
#define PAGED_CODE() ((void)0)
#define NT_SUCCESS(status) ((status) >= 0)
#define DXGKDDI_INTERFACE_VERSION 0x3000
#define DXGKDDI_INTERFACE_VERSION_WDDM2_3 0x2300
#define RtlZeroMemory(p, size) std::memset(p, 0, size)
enum { DXGK_INTERRUPT_CRTC_VSYNC, DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY2 };
struct DXGK_MULTIPLANE_OVERLAY_VSYNC_INFO2 { ULONGLONG PresentId; };
struct DXGKARGCB_NOTIFY_INTERRUPT_DATA {
    int InterruptType;
    struct { unsigned VidPnTargetId; LARGE_INTEGER PhysicalAddress; unsigned PhysicalAdapterMask; } CrtcVsync;
    struct {
        unsigned VidPnTargetId, PhysicalAdapterMask, MultiPlaneOverlayVsyncInfoCount;
        DXGK_MULTIPLANE_OVERLAY_VSYNC_INFO2* pMultiPlaneOverlayVsyncInfo;
    } CrtcVsyncWithMultiPlaneOverlay2;
};
struct DXGKARG_GETSCANLINE {
    unsigned VidPnTargetId = 0, ScanLine = 0;
    bool InVerticalBlank = false;
};
static std::mutex interlockedMutex;
LONG InterlockedExchange(volatile LONG* p, LONG v) {
    std::lock_guard lock(interlockedMutex); const LONG previous = *p; *p = v; return previous;
}
LONG InterlockedIncrement(volatile LONG* p) {
    std::lock_guard lock(interlockedMutex); return ++*p;
}
LONG64 InterlockedExchange64(volatile LONG64* p, LONG64 v) {
    std::lock_guard lock(interlockedMutex); const LONG64 previous = *p; *p = v; return previous;
}
LONG64 InterlockedCompareExchange64(volatile LONG64* p, LONG64 v, LONG64 expected) {
    std::lock_guard lock(interlockedMutex); const LONG64 previous = *p;
    if (previous == expected) *p = v;
    return previous;
}
LONG InterlockedCompareExchange(volatile LONG* p, LONG v, LONG expected) {
    std::lock_guard lock(interlockedMutex); const LONG previous = *p;
    if (previous == expected) *p = v;
    return previous;
}
void ExAcquireFastMutex(FAST_MUTEX* m) { m->lock(); }
void ExReleaseFastMutex(FAST_MUTEX* m) { m->unlock(); }
void KeAcquireSpinLock(KSPIN_LOCK* m, KIRQL* level) { *level = 0; m->lock(); }
static thread_local std::function<void()> afterUnlock;
void KeReleaseSpinLock(KSPIN_LOCK* m, KIRQL) {
    m->unlock();
    auto hook = std::move(afterUnlock);
    afterUnlock = {};
    if (hook) hook();
}
static std::atomic<LONGLONG> counter{0}, counterFrequency{19200000};
LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER* frequency) {
    if (frequency) frequency->QuadPart = counterFrequency.load();
    return {counter.load()};
}

struct Statistics {
    std::atomic<bool> disabled{false}, deleted{false};
    std::atomic<unsigned> sets{0}, rejected{0};
};
struct Timer;
using PEX_TIMER = Timer*;
struct Timer {
    std::mutex mutex;
    std::condition_variable changed;
    bool pending = false;
    unsigned callbacks = 0;
    LONGLONG due = 0, period = 0;
    VOID (*callback)(PEX_TIMER, PVOID);
    PVOID context;
    std::shared_ptr<Statistics> stats = std::make_shared<Statistics>();
};
static bool failAllocation;
static std::function<void()> beforeSet, insideDelivery, insideAllocation;
PEX_TIMER ExAllocateTimer(VOID (*callback)(PEX_TIMER, PVOID), PVOID context, int attributes) {
    assert(attributes == EX_TIMER_HIGH_RESOLUTION);
    if (insideAllocation) insideAllocation();
    if (failAllocation) { failAllocation = false; return nullptr; }
    auto* timer = new Timer;
    timer->callback = callback;
    timer->context = context;
    return timer;
}
bool ExSetTimer(PEX_TIMER timer, LONGLONG due, LONGLONG period, PVOID) {
    if (beforeSet) beforeSet();
    std::lock_guard lock(timer->mutex);
    assert(!timer->stats->deleted && due < 0 && period == 0);
    if (timer->stats->disabled) { ++timer->stats->rejected; return false; }
    const bool previous = timer->pending;
    timer->pending = true;
    timer->due = due;
    timer->period = period;
    ++timer->stats->sets;
    return previous; // FALSE can mean a successful new timer operation.
}
bool ExDeleteTimer(PEX_TIMER timer, bool cancel, bool wait, PVOID) {
    assert(cancel && wait);
    std::unique_lock lock(timer->mutex);
    timer->stats->disabled = true;
    const bool previous = timer->pending;
    timer->pending = false;
    timer->changed.wait(lock, [&] { return timer->callbacks == 0; });
    timer->stats->deleted = true;
    lock.unlock();
    delete timer;
    return previous;
}
static void fire(PEX_TIMER timer) {
    {
        std::lock_guard lock(timer->mutex);
        assert(timer->pending && !timer->stats->disabled);
        timer->pending = false;
        ++timer->callbacks;
    }
    timer->callback(timer, timer->context);
    {
        std::lock_guard lock(timer->mutex);
        --timer->callbacks;
        timer->changed.notify_all();
    }
}
struct VioGpuAdapter {
    unsigned refreshes = 0;
    void RequestScanoutRefresh() { ++refreshes; }
};
struct VioGpuDod {
    volatile LONG m_CrtcVsyncTimerArmed = 0;
    volatile LONG m_CrtcVsyncEnabled = 1, m_CrtcVsyncDeliveredCount = 0;
    volatile LONG64 m_CrtcVsyncPrimaryAddress = 23, m_CrtcLastVsyncTicks = 0;
    volatile LONG m_ColorPresentActive = 0, m_ColorPresentCompletedEpoch = 0;
    volatile LONG64 m_ColorPresentCompletedId = 0;
    PEX_TIMER m_CrtcVsyncTimer = nullptr;
    FAST_MUTEX m_CrtcTimerMutex;
    KSPIN_LOCK m_CrtcTimingLock;
    VIOGPU_DISPLAY_TIMING m_CrtcTiming = VioGpuVirtualTiming(1920, 1080, 165);
    struct { struct { bool FrameBufferIsActive = true, SourceNotVisible = false; } Flags; } m_CurrentMode;
    LONGLONG m_CrtcEpoch = 0, m_CrtcPeriodTicks = 0, m_CrtcNextDueTicks = 0;
    VIOGPU_VBLANK_CADENCE_COUNTERS m_CrtcVblankCadence{};
    LONGLONG m_CrtcAdapterStartQpc = 1234;
    unsigned delivered = 0, late = 0;
    bool interruptAllowed = true, notifyAccepted = true;
    VioGpuAdapter hardware;
    VioGpuAdapter* m_pHWDevice = &hardware;
    NTSTATUS ArmCrtcVsyncTimer();
    NTSTATUS GetScanLine(DXGKARG_GETSCANLINE*);
    NTSTATUS SetCrtcTiming(const VIOGPU_DISPLAY_TIMING&);
    bool IsDriverActive() const { return true; }
    bool IsHardwareInit() const { return true; }
    BOOLEAN CrtcVsyncDue();
    VOID RearmCrtcVsyncTimer(PEX_TIMER);
    VOID DisarmCrtcVsyncTimer();
    VOID CountDisplayEvent(int) { ++late; }
    BOOLEAN IsHardwareInterruptDispatchAllowed() const { return interruptAllowed; }
    ULONG QueryNativeFenceEpoch() const { return 1; }
    BOOLEAN NotifyNativeSchedulerInterrupt(const DXGKARGCB_NOTIFY_INTERRUPT_DATA*, BOOLEAN, ULONG) {
        if (!notifyAccepted) return false;
        ++delivered;
        if (insideDelivery) insideDelivery();
        return true;
    }
    VOID DeliverCrtcVsync();
    VOID RecordCrtcVblankDelivery(VIOGPU_VBLANK_DELIVERY_OUTCOME);
    VOID ReadCrtcVblankCadence(VIOGPU_VBLANK_CADENCE_SNAPSHOT&);
    NTSTATUS PublishCrtcVblankCadence(HANDLE);
};
// INSERT_CALLBACK
// INSERT_METHODS
// INSERT_DELIVERY
// INSERT_SCANLINE
// INSERT_TIMING

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool arrived = false, open = false;
    void pause() {
        std::unique_lock lock(mutex);
        arrived = true;
        changed.notify_all();
        changed.wait(lock, [&] { return open; });
    }
    void wait() {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(2), [&] { return arrived; }));
    }
    void release() { std::lock_guard lock(mutex); open = true; changed.notify_all(); }
};
static void cancellationRace(bool pauseBeforeSet) {
    VioGpuDod adapter;
    counter = 0;
    assert(adapter.ArmCrtcVsyncTimer() == STATUS_SUCCESS);
    auto* timer = adapter.m_CrtcVsyncTimer;
    auto stats = timer->stats;
    counter = adapter.m_CrtcNextDueTicks;
    Gate gate;
    if (pauseBeforeSet) beforeSet = [&] { gate.pause(); };
    else insideDelivery = [&] { gate.pause(); };
    std::thread callback([&] { fire(timer); });
    gate.wait();
    std::atomic<bool> stopped{false}, restarted{false};
    std::thread disarm([&] { adapter.DisarmCrtcVsyncTimer(); stopped = true; });
    const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!stats->disabled && std::chrono::steady_clock::now() < timeout) std::this_thread::yield();
    assert(stats->disabled && !stats->deleted && !stopped);
    // A new timer cannot be published until old callback rundown is complete.
    std::thread arm([&] { assert(adapter.ArmCrtcVsyncTimer() == STATUS_SUCCESS); restarted = true; });
    assert(!restarted);
    gate.release();
    callback.join();
    disarm.join();
    arm.join();
    beforeSet = {};
    insideDelivery = {};
    assert(stopped && restarted && stats->deleted);
    assert(stats->sets == 1 && stats->rejected == (pauseBeforeSet ? 1U : 0U));
    assert(adapter.m_CrtcVsyncTimer->stats != stats);
    assert(adapter.m_CrtcVsyncTimer->stats->sets == 1);
    adapter.DisarmCrtcVsyncTimer();
}
static void productionCadence(bool extraHalfMillisecond) {
    VioGpuDod adapter;
    counter = 0;
    const LONGLONG frequency = counterFrequency.load(), quantum = frequency / 2000;
    assert(adapter.ArmCrtcVsyncTimer() == STATUS_SUCCESS);
    auto* timer = adapter.m_CrtcVsyncTimer;
    auto stats = timer->stats;
    unsigned callbacks = 0;
    insideDelivery = [&] { counter += (adapter.delivered % 5) * frequency / 20000; };
    for (;;) {
        // Use the actual ExSetTimer arguments from the production rearm body,
        // then impose a 0.5 ms kernel clock grid and variable callback latency.
        const LONGLONG ticks = ((-timer->due) * frequency + 9999999) / 10000000;
        const LONGLONG expiry = ((counter.load() + ticks + quantum - 1) / quantum) * quantum +
                               (extraHalfMillisecond ? quantum : 0);
        if (expiry > frequency * 100) break;
        counter = expiry;
        fire(timer);
        ++callbacks;
    }
    insideDelivery = {};
    std::printf("PRODUCTION_ONE_SHOT 100s qpc=%lld extra_delay_us=%u callbacks=%u vblanks=%u old_sampler_callbacks=200000\n",
                frequency, extraHalfMillisecond ? 500U : 0U, callbacks, adapter.delivered);
    std::fflush(stdout);
    if (adapter.delivered < 16499 || adapter.delivered > 16500 || callbacks != adapter.delivered) {
        std::puts("FAIL phase-aligned cadence");
        adapter.DisarmCrtcVsyncTimer();
        std::exit(1);
    }
    assert(stats->sets == callbacks + 1);
    adapter.DisarmCrtcVsyncTimer();
}
static unsigned expectedLine(const VIOGPU_DISPLAY_TIMING& timing, LONGLONG phase, LONGLONG period) {
    return (static_cast<ULONGLONG>(phase) * timing.TotalHeight / period + timing.Height) % timing.TotalHeight;
}
static void requireRaster(VioGpuDod& adapter, LONGLONG phase, const char* failure) {
    DXGKARG_GETSCANLINE scan;
    const unsigned expected = expectedLine(adapter.m_CrtcTiming, phase, adapter.m_CrtcPeriodTicks);
    if (adapter.GetScanLine(&scan) != STATUS_SUCCESS || scan.ScanLine != expected ||
        scan.InVerticalBlank != (expected >= adapter.m_CrtcTiming.Height)) {
        std::puts(failure);
        std::fflush(stdout);
        std::exit(1);
    }
}
static void productionRaster() {
    VioGpuDod adapter;
    counter = 19200000;
    counterFrequency = 19200000;
    assert(adapter.ArmCrtcVsyncTimer() == STATUS_SUCCESS);
    auto* timer = adapter.m_CrtcVsyncTimer;
    const LONGLONG period = adapter.m_CrtcPeriodTicks, firstDue = adapter.m_CrtcNextDueTicks;
    requireRaster(adapter, 0, "FAIL initial raster");
    counter = firstDue - 1;
    requireRaster(adapter, period - 1, "FAIL pre-deadline raster");
    counter = firstDue;
    requireRaster(adapter, 0, "FAIL due raster before callback");
    counter = firstDue + period / 4;
    requireRaster(adapter, period / 4, "FAIL delayed raster before callback");
    fire(timer); // Arrival resets epoch but must not shift the fixed-grid raster.
    requireRaster(adapter, period / 4, "FAIL armed raster grid");
    counter = adapter.m_CrtcNextDueTicks - 1;
    requireRaster(adapter, period - 1, "FAIL armed raster before next deadline");
    counter = adapter.m_CrtcNextDueTicks;
    requireRaster(adapter, 0, "FAIL armed raster next deadline");
    // Before a stalled DPC arrives, raster keeps wrapping on the current grid.
    counter = adapter.m_CrtcNextDueTicks + period * 4 + period / 3;
    requireRaster(adapter, period / 3, "FAIL delayed multi-frame raster");
    fire(timer); // Skipped whole frames must preserve the original raster phase.
    requireRaster(adapter, period / 3, "FAIL stalled grid-preserving raster");
    adapter.DisarmCrtcVsyncTimer();
    counter = adapter.m_CrtcEpoch + period / 2;
    requireRaster(adapter, period / 2, "FAIL disabled epoch raster");

    // Query during Arm after a mode change: old due must not mix with new period.
    unsigned armQueries = 0;
    insideAllocation = [&] {
        ++armQueries;
        DXGKARG_GETSCANLINE scan;
        if (adapter.GetScanLine(&scan) != STATUS_DEVICE_NOT_READY) {
            std::puts("FAIL unpublished arm raster");
            std::fflush(stdout);
            std::exit(1);
        }
    };
    const auto nextMode = VioGpuVirtualTiming(3040, 1904, 120);
    assert(adapter.SetCrtcTiming(nextMode) == STATUS_SUCCESS);
    insideAllocation = {};
    assert(armQueries == 1);
    requireRaster(adapter, 0, "FAIL mode-change raster origin");
    counter = adapter.m_CrtcNextDueTicks - adapter.m_CrtcPeriodTicks / 2;
    const LONGLONG newPhase = adapter.m_CrtcPeriodTicks - adapter.m_CrtcPeriodTicks / 2;
    requireRaster(adapter, newPhase, "FAIL mode-change raster phase");

    // A concurrent mode publication just after GetScanLine unlocks must not
    // combine any new due/period/geometry/clock with the captured old snapshot.
    const auto oldMode = adapter.m_CrtcTiming;
    const auto oldPeriod = adapter.m_CrtcPeriodTicks;
    const auto oldExpected = expectedLine(oldMode, newPhase, oldPeriod);
    afterUnlock = [&] {
        assert(adapter.SetCrtcTiming(VioGpuVirtualTiming(1024, 768, 60)) == STATUS_SUCCESS);
    };
    DXGKARG_GETSCANLINE scan;
    if (adapter.GetScanLine(&scan) != STATUS_SUCCESS || scan.ScanLine != oldExpected ||
        scan.InVerticalBlank != (oldExpected >= oldMode.Height)) {
        std::puts("FAIL coherent raster snapshot");
        std::fflush(stdout);
        std::exit(1);
    }
    requireRaster(adapter, 0, "FAIL following mode snapshot");
    // Invalid mode is rejected while preserving the current armed grid.
    const auto dueBeforeInvalid = adapter.m_CrtcNextDueTicks;
    auto invalid = adapter.m_CrtcTiming;
    invalid.PixelClock = 0;
    assert(adapter.SetCrtcTiming(invalid) == STATUS_INVALID_PARAMETER);
    assert(adapter.m_CrtcNextDueTicks == dueBeforeInvalid);
    requireRaster(adapter, 0, "FAIL refused mode raster");
    // Allocation failure rolls back to the previous mode and publishes a fresh grid.
    const auto widthBeforeFailed = adapter.m_CrtcTiming.Width;
    failAllocation = true;
    assert(adapter.SetCrtcTiming(nextMode) == STATUS_INSUFFICIENT_RESOURCES);
    assert(adapter.m_CrtcTiming.Width == widthBeforeFailed && adapter.m_CrtcVsyncTimerArmed);
    requireRaster(adapter, 0, "FAIL rolled-back mode raster");
    adapter.DisarmCrtcVsyncTimer();
    adapter.m_CrtcVsyncEnabled = 0;
    assert(adapter.SetCrtcTiming(nextMode) == STATUS_SUCCESS);
    counter += adapter.m_CrtcPeriodTicks / 3;
    requireRaster(adapter, adapter.m_CrtcPeriodTicks / 3, "FAIL disabled new-mode epoch");
    adapter.m_CrtcPeriodTicks = 0;
    assert(adapter.GetScanLine(&scan) == STATUS_DEVICE_NOT_READY);
    assert(adapter.GetScanLine(nullptr) == STATUS_DEVICE_NOT_READY);
    std::puts("PASS production raster: late callback/deadline/stall/disabled/modechange/arm-publication/snapshot/rollback");
}
static void requireCadence(bool good, const char* failure) {
    if (good) return;
    std::puts(failure);
    std::fflush(stdout);
    std::exit(1);
}
static void productionCadenceDiagnostics() {
    VioGpuDod adapter;
    counter = 19200000;
    assert(adapter.ArmCrtcVsyncTimer() == STATUS_SUCCESS);
    auto* timer = adapter.m_CrtcVsyncTimer;
    const LONGLONG period = adapter.m_CrtcPeriodTicks;
    const LONGLONG firstDue = adapter.m_CrtcNextDueTicks;
    assert(adapter.ArmCrtcVsyncTimer() == STATUS_SUCCESS);
    assert(adapter.m_CrtcVblankCadence.ArmCount == 1);
    counter = adapter.m_CrtcNextDueTicks + period / 2;
    fire(timer);
    counter = adapter.m_CrtcNextDueTicks - 1;
    fire(timer);
    adapter.m_CrtcPeriodTicks = 0;
    fire(timer);
    adapter.m_CrtcPeriodTicks = period;
    const auto oldDue = adapter.m_CrtcNextDueTicks;
    const ULONGLONG late = period * 2 + period / 3;
    counter = oldDue + late;
    fire(timer);
    requireCadence(adapter.m_CrtcNextDueTicks == oldDue + 3 * period &&
                   adapter.m_CrtcVblankCadence.ResyncCount == 1 &&
                   adapter.m_CrtcVblankCadence.MissedWholePeriods == 2 &&
                   adapter.m_CrtcVblankCadence.ResyncPhaseTicks == 2 * static_cast<ULONGLONG>(period) &&
                   adapter.m_CrtcVblankCadence.MaxLatenessTicks == late,
                   "FAIL cadence resync accounting");
    counter = adapter.m_CrtcNextDueTicks + period;
    fire(timer); // An exact full-period delay also resynchronizes.
    assert(adapter.m_CrtcVblankCadence.ResyncCount == 2);
    assert(adapter.m_CrtcVblankCadence.MissedWholePeriods == 3);
    assert(adapter.m_CrtcVblankCadence.ResyncPhaseTicks == 3 * static_cast<ULONGLONG>(period));
    assert(adapter.m_CrtcVblankCadence.MaxLatenessTicks == late);
    const auto next = [&] { counter = adapter.m_CrtcNextDueTicks; fire(timer); };
    adapter.m_CrtcVsyncEnabled = 0;
    next();
    adapter.m_CrtcVsyncEnabled = 1;
    adapter.interruptAllowed = false;
    next();
    adapter.interruptAllowed = true;
    adapter.m_ColorPresentActive = 1;
    next();
    adapter.m_ColorPresentActive = 0;
    adapter.notifyAccepted = false;
    next();
    adapter.notifyAccepted = true;
    insideDelivery = [&] {
        VIOGPU_VBLANK_CADENCE_SNAPSHOT during;
        adapter.ReadCrtcVblankCadence(during);
        ULONGLONG outcomes = 0;
        for (auto count : during.Counters.Delivery) outcomes += count;
        assert(during.Counters.DueCount == outcomes + 1); // Current callback in flight.
    };
    next();
    insideDelivery = {};
    auto& metrics = adapter.m_CrtcVblankCadence;
    requireCadence(metrics.Delivery[VioGpuVblankDisabled] == 1 &&
                   metrics.Delivery[VioGpuVblankHardwareGated] == 1 &&
                   metrics.Delivery[VioGpuVblankColorGated] == 1 &&
                   metrics.Delivery[VioGpuVblankNotifyFailed] == 1 &&
                   metrics.Delivery[VioGpuVblankDelivered] == adapter.delivered,
                   "FAIL cadence gate accounting");
    assert(metrics.CallbackCount == metrics.DueCount + metrics.EarlyCount + metrics.InvalidPeriodCount);
    assert(metrics.CallbackCount == 10 && metrics.DueCount == 8 && metrics.EarlyCount == 1 && metrics.InvalidPeriodCount == 1);
    requireCadence(static_cast<ULONGLONG>(adapter.m_CrtcNextDueTicks - firstDue) ==
                   period * metrics.DueCount + metrics.ResyncPhaseTicks,
                   "FAIL cadence deadline conservation");
    VIOGPU_VBLANK_CADENCE_SNAPSHOT before;
    adapter.ReadCrtcVblankCadence(before);
    assert(before.Version == 1 && before.Size == 208 && before.QpcFrequency == 19200000);
    assert(before.AdapterStartQpc == 1234 && before.SnapshotQpc == static_cast<ULONGLONG>(counter));
    afterUnlock = [&] {
        counter = adapter.m_CrtcNextDueTicks;
        fire(timer);
        assert(adapter.SetCrtcTiming(VioGpuVirtualTiming(3040, 1904, 120)) == STATUS_SUCCESS);
    };
    VIOGPU_VBLANK_CADENCE_SNAPSHOT captured;
    adapter.ReadCrtcVblankCadence(captured);
    requireCadence(std::memcmp(&before, &captured, sizeof(before)) == 0,
                   "FAIL cadence coherent snapshot");
    // The registry write must publish this captured sample even as the next
    // callback/mode proceeds, and a failure must not report success.
    adapter.ReadCrtcVblankCadence(before);
    insidePublish = [&] { counter = adapter.m_CrtcNextDueTicks; fire(adapter.m_CrtcVsyncTimer); };
    requireCadence(adapter.PublishCrtcVblankCadence(nullptr) == STATUS_SUCCESS && cadencePublications == 1 &&
                   std::memcmp(&before, &publishedCadence, sizeof(before)) == 0,
                   "FAIL cadence atomic publication");
    insidePublish = {};
    publicationStatus = STATUS_INSUFFICIENT_RESOURCES;
    assert(adapter.PublishCrtcVblankCadence(nullptr) == publicationStatus && cadencePublications == 1);
    publicationStatus = STATUS_SUCCESS;
    adapter.DisarmCrtcVsyncTimer();
    adapter.DisarmCrtcVsyncTimer();
    assert(metrics.ArmCount == 2 && metrics.DisarmCount == 2 && metrics.ModeChanges == 1);
    std::puts("PASS production cadence diagnostics: resync/gates/lifecycle/coherent snapshot/atomic publication");
}
static void productionCadenceGrid() {
    VioGpuDod adapter;
    counter = 19200000;
    assert(adapter.ArmCrtcVsyncTimer() == STATUS_SUCCESS);
    auto* timer = adapter.m_CrtcVsyncTimer;
    const auto origin = adapter.m_CrtcNextDueTicks;
    const auto period = adapter.m_CrtcPeriodTicks;
    ULONGLONG missed = 0, resyncs = 0;
    for (unsigned i = 0; i < 2000; ++i) {
        const auto dueAt = adapter.m_CrtcNextDueTicks;
        const unsigned skipped = i % 9;
        const LONGLONG fraction = (i * 7919) % period;
        counter = dueAt + skipped * period + fraction;
        fire(timer);
        missed += skipped;
        resyncs += skipped != 0;
        const auto next = adapter.m_CrtcNextDueTicks;
        const auto& counts = adapter.m_CrtcVblankCadence;
        requireCadence(next > counter && next - period <= counter && (next - origin) % period == 0 &&
                       counts.DueCount == i + 1 && adapter.delivered == i + 1 &&
                       counts.ResyncCount == resyncs && counts.MissedWholePeriods == missed &&
                       counts.ResyncPhaseTicks == missed * period &&
                       static_cast<ULONGLONG>(next - origin) == period * (counts.DueCount + missed),
                       "FAIL cadence repeated grid conservation");
        fire(timer); // An immediate duplicate must not fabricate a missed vblank.
        requireCadence(adapter.m_CrtcNextDueTicks == next && adapter.delivered == i + 1 &&
                       counts.EarlyCount == i + 1,
                       "FAIL cadence repeated no-burst");
    }
    adapter.DisarmCrtcVsyncTimer();
    std::puts("PASS production grid: 2000 mixed whole/fractional stalls, exact conservation, no bursts");
}
static void productionCadenceLimits() {
    VioGpuDod adapter;
    constexpr LONGLONG maximum = 0x7fffffffffffffffLL, minimum = -maximum - 1;
    adapter.m_CrtcPeriodTicks = 1;
    adapter.m_CrtcNextDueTicks = maximum - 2;
    counter = maximum - 1;
    assert(adapter.CrtcVsyncDue() && adapter.m_CrtcNextDueTicks == maximum);
    counter = maximum;
    for (unsigned i = 0; i < 2; ++i) {
        assert(!adapter.CrtcVsyncDue() && adapter.m_CrtcNextDueTicks == maximum);
    }
    const auto& counts = adapter.m_CrtcVblankCadence;
    requireCadence(counts.CallbackCount == 3 && counts.DueCount == 1 &&
                   counts.InvalidPeriodCount == 2 && counts.EarlyCount == 0 &&
                   counts.ResyncCount == 1 && counts.MissedWholePeriods == 1 && counts.ResyncPhaseTicks == 1,
                   "FAIL cadence unrepresentable deadline");
    VioGpuDod crossing;
    crossing.m_CrtcPeriodTicks = 3;
    crossing.m_CrtcNextDueTicks = minimum;
    counter = 0;
    assert(crossing.CrtcVsyncDue() && crossing.m_CrtcNextDueTicks == 1);
    const ULONGLONG late = 1ULL << 63;
    requireCadence(crossing.m_CrtcVblankCadence.MaxLatenessTicks == late && crossing.late == 1 &&
                   crossing.m_CrtcVblankCadence.MissedWholePeriods == late / 3 &&
                   crossing.m_CrtcVblankCadence.ResyncPhaseTicks == late - late % 3,
                   "FAIL cadence unsigned lateness");
    std::puts("PASS production cadence: signed-limit rejection/classification and full-width unsigned lateness");
}
int main() {
    productionRaster();
    productionCadence(false);
    productionCadence(true);
    VioGpuDod adapter;
    failAllocation = true;
    assert(adapter.ArmCrtcVsyncTimer() == STATUS_INSUFFICIENT_RESOURCES);
    assert(adapter.m_CrtcVsyncTimer == nullptr && adapter.m_CrtcVsyncTimerArmed == 0);
    adapter.m_CrtcTiming.PixelClock = 0;
    assert(adapter.ArmCrtcVsyncTimer() == STATUS_INVALID_PARAMETER);
    adapter.m_CrtcTiming = VioGpuVirtualTiming(1920, 1080, 165);
    counterFrequency = 0;
    assert(adapter.ArmCrtcVsyncTimer() == STATUS_INVALID_PARAMETER);
    counterFrequency = 19200000;
    assert(adapter.ArmCrtcVsyncTimer() == STATUS_SUCCESS);
    auto* timer = adapter.m_CrtcVsyncTimer;
    auto stats = timer->stats;
    const auto deadline = adapter.m_CrtcNextDueTicks;
    counter = deadline - 1;
    fire(timer); // Even an early wakeup must not invent a vblank or shift phase.
    assert(adapter.delivered == 0 && adapter.m_CrtcNextDueTicks == deadline);
    counter = deadline;
    fire(timer);
    assert(adapter.delivered == 1 && stats->sets == 3);
    // Actual delivery body observes the enable gate without discarding phase.
    InterlockedExchange(&adapter.m_CrtcVsyncEnabled, 0);
    counter = adapter.m_CrtcNextDueTicks;
    fire(timer);
    if (adapter.delivered != 1 || !timer->pending) {
        std::puts("FAIL disabled vblank delivery");
        adapter.DisarmCrtcVsyncTimer();
        return 1;
    }
    InterlockedExchange(&adapter.m_CrtcVsyncEnabled, 1);
    counter = adapter.m_CrtcNextDueTicks;
    fire(timer);
    assert(adapter.delivered == 2);
    adapter.interruptAllowed = false;
    counter = adapter.m_CrtcNextDueTicks;
    fire(timer);
    assert(adapter.delivered == 2 && timer->pending);
    adapter.interruptAllowed = true;
    // A multi-frame stall produces one vblank, then moves nextDue forward.
    counter = adapter.m_CrtcNextDueTicks + 5 * adapter.m_CrtcPeriodTicks;
    const LONGLONG stalled = counter.load();
    fire(timer);
    assert(adapter.delivered == 3);
    assert(adapter.m_CrtcNextDueTicks == stalled + adapter.m_CrtcPeriodTicks);
    fire(timer); // Immediate repeated/early callback cannot catch up in a burst.
    assert(adapter.delivered == 3 && adapter.m_CrtcNextDueTicks == stalled + adapter.m_CrtcPeriodTicks);
    adapter.DisarmCrtcVsyncTimer();
    assert(stats->deleted && adapter.m_CrtcVsyncTimer == nullptr);
    adapter.DisarmCrtcVsyncTimer();
    for (unsigned i = 0; i < 32; ++i) {
        cancellationRace(false);
        cancellationRace(true);
    }
    std::puts("PASS production vblank timer: delayed phase/early wake/stall/enable gates/delete/rearm; 64 forced cancellation races");
    productionCadenceDiagnostics();
    productionCadenceGrid();
    productionCadenceLimits();
}
