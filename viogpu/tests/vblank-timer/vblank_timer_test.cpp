// Production callback/arm/due/rearm/deliver/disarm bodies, with an EX_TIMER peer that
// disables new operations and waits for callbacks before deleting the object.
// This exercises driver ordering; it is not a Windows kernel execution test.
#include "display_timing.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

using LONG = int32_t;
using ULONG = uint32_t;
using ULONGLONG = unsigned long long;
using LONG64 = long long;
using LONGLONG = long long;
using VOID = void;
using PVOID = void*;
using BOOLEAN = bool;
using NTSTATUS = int32_t;
using KIRQL = int;
using FAST_MUTEX = std::mutex;
using KSPIN_LOCK = std::mutex;
struct LARGE_INTEGER { LONGLONG QuadPart; };
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_INVALID_PARAMETER = -1, STATUS_INSUFFICIENT_RESOURCES = -2;
constexpr int EX_TIMER_HIGH_RESOLUTION = 1, VioGpuVsyncLateOver1ms = 1;
#define _In_
#define _In_opt_
#define TRUE true
#define FALSE false
#define PAGED_CODE() ((void)0)
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
void KeReleaseSpinLock(KSPIN_LOCK* m, KIRQL) { m->unlock(); }
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
static std::function<void()> beforeSet, insideDelivery;
PEX_TIMER ExAllocateTimer(VOID (*callback)(PEX_TIMER, PVOID), PVOID context, int attributes) {
    assert(attributes == EX_TIMER_HIGH_RESOLUTION);
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
    LONGLONG m_CrtcEpoch = 0, m_CrtcPeriodTicks = 0, m_CrtcNextDueTicks = 0;
    unsigned delivered = 0, late = 0;
    bool interruptAllowed = true;
    VioGpuAdapter hardware;
    VioGpuAdapter* m_pHWDevice = &hardware;
    NTSTATUS ArmCrtcVsyncTimer();
    BOOLEAN CrtcVsyncDue();
    VOID RearmCrtcVsyncTimer(PEX_TIMER);
    VOID DisarmCrtcVsyncTimer();
    VOID CountDisplayEvent(int) { ++late; }
    BOOLEAN IsHardwareInterruptDispatchAllowed() const { return interruptAllowed; }
    ULONG QueryNativeFenceEpoch() const { return 1; }
    BOOLEAN NotifyNativeSchedulerInterrupt(const DXGKARGCB_NOTIFY_INTERRUPT_DATA*, BOOLEAN, ULONG) {
        ++delivered;
        if (insideDelivery) insideDelivery();
        return true;
    }
    VOID DeliverCrtcVsync();
};
// INSERT_CALLBACK
// INSERT_METHODS
// INSERT_DELIVERY

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
int main() {
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
}
