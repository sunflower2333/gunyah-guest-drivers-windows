#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>
#include "display_timing.h"

// CLOCK

#define _In_
#define _In_opt_
#define PAGED_CODE() ((void)0)
using VOID = void;
using PVOID = void *;
using LONG = long;
using LONGLONG = long long;
using ULONGLONG = unsigned long long;
using ULONG64 = unsigned long long;
using KIRQL = int;
using NTSTATUS = int;
using FAST_MUTEX = std::mutex;
using KSPIN_LOCK = std::mutex;
struct LARGE_INTEGER { LONGLONG QuadPart; };
struct Timer;
using PEX_TIMER = Timer *;
using Callback = void (*)(PEX_TIMER, PVOID);
struct Timer {
    Callback callback;
    PVOID context;
    ULONGLONG due = 0;
    bool pending = false, disabled = false, deleted = false;
};
constexpr bool TRUE = true;
constexpr int STATUS_SUCCESS = 0, STATUS_INVALID_PARAMETER = -1, STATUS_INSUFFICIENT_RESOURCES = -2;
constexpr unsigned EX_TIMER_HIGH_RESOLUTION = 4;
static ULONGLONG now100ns = 10000000;
static std::vector<std::unique_ptr<Timer>> timers;
static unsigned sets, deletions, deliveries, failures;
static bool failAllocation;
static void Check(bool ok, const char *name) {
    if (!ok) {
        if (failures < 8) std::printf("FAIL %s\n", name);
        ++failures;
    }
}
static LONG InterlockedExchange(volatile LONG *p, LONG v) { LONG old = *p; *p = v; return old; }
[[maybe_unused]] static LONG InterlockedCompareExchange(volatile LONG *p, LONG v, LONG expected) {
    LONG old = *p; if (old == expected) *p = v; return old;
}
static void ExAcquireFastMutex(FAST_MUTEX *p) { p->lock(); }
static void ExReleaseFastMutex(FAST_MUTEX *p) { p->unlock(); }
static void KeAcquireSpinLock(KSPIN_LOCK *p, KIRQL *old) { *old = 0; p->lock(); }
static void KeReleaseSpinLock(KSPIN_LOCK *p, KIRQL) { p->unlock(); }
static ULONGLONG KeQueryInterruptTimePrecise(ULONG64 *qpc) {
    *qpc = now100ns; return now100ns;
}
static LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER *frequency) {
    if (frequency) frequency->QuadPart = 10000000;
    return {static_cast<LONGLONG>(now100ns)};
}
static PEX_TIMER ExAllocateTimer(Callback callback, PVOID context, unsigned flags) {
    Check(flags == EX_TIMER_HIGH_RESOLUTION, "high-resolution timer");
    if (failAllocation) return nullptr;
    timers.emplace_back(new Timer{callback, context});
    return timers.back().get();
}
static bool ExSetTimer(PEX_TIMER timer, LONGLONG due, LONGLONG period, PVOID) {
    Check(!timer->deleted, "no freed timer use");
    Check(due < 0 && period == 0, "relative one-shot only");
    if (timer->disabled) return false;
    ++sets;
    timer->due = now100ns + static_cast<ULONGLONG>(-due);
    timer->pending = true;
    return false;
}
static bool ExDeleteTimer(PEX_TIMER timer, bool cancel, bool wait, PVOID) {
    Check(cancel && wait, "teardown waits for callbacks");
    Check(!timer->deleted, "timer deleted once");
    timer->disabled = true;
    const auto beforeSets = sets, beforeDeliveries = deliveries;
    // Model the last queued callback racing deletion after the admission gate
    // closes. The real API keeps its callback argument alive until return.
    timer->callback(timer, timer->context);
    Check(sets == beforeSets && deliveries == beforeDeliveries, "no callback admitted during deletion");
    timer->deleted = true;
    timer->pending = false;
    ++deletions;
    return true;
}
struct VioGpuDod {
    PEX_TIMER m_CrtcVsyncTimer = nullptr;
    FAST_MUTEX m_CrtcTimerMutex;
    KSPIN_LOCK m_CrtcTimingLock;
    VIOGPU_DISPLAY_TIMING m_CrtcTiming = VioGpuVirtualTiming(3040, 1904, 165);
    VIOGPU_VBLANK_CLOCK m_CrtcVblankClock = {};
    LONGLONG m_CrtcPeriodTicks = 0, m_CrtcEpoch = 0;
    volatile LONG m_CrtcVsyncTimerArmed = 0;
    bool stopInDelivery = false;
    ULONGLONG work100ns = 0;
    void DeliverCrtcVsync() {
        ++deliveries;
        now100ns += work100ns;
        if (stopInDelivery) InterlockedExchange(&m_CrtcVsyncTimerArmed, 0);
    }
    VOID OnCrtcVsyncTimer(PEX_TIMER timer);
    NTSTATUS ArmCrtcVsyncTimer(void);
    VOID DisarmCrtcVsyncTimer(void);
};

// PRODUCTION

static void Fire(VioGpuDod &dod, ULONGLONG lateness) {
    auto *timer = dod.m_CrtcVsyncTimer;
    Check(timer && timer->pending && !timer->disabled, "one pending timer");
    now100ns = timer->due + lateness;
    timer->pending = false;
    timer->callback(timer, &dod);
}

int main() {
    VIOGPU_VBLANK_CLOCK clock = {};
    Check(!VioGpuStartVblankClock(0, 0, clock), "zero period");
    Check(!VioGpuStartVblankClock(0, 0x80000000ULL, clock), "oversize period");
    Check(!VioGpuStartVblankClock(~0ULL, 1, clock), "start overflow");
    ULONGLONG delay = 0;
    Check(!VioGpuNextVblankDeadline(0, clock, delay), "uninitialized clock");
    for (unsigned hz : {24U, 60U, 97U, 120U, 144U, 165U, 240U}) {
        const ULONGLONG period = (10000000ULL + hz / 2) / hz;
        const ULONGLONG start = 10000000ULL;
        Check(VioGpuStartVblankClock(start, period, clock), "start grid");
        for (unsigned i = 1; i <= 10000; ++i) {
            ULONGLONG sample = clock.Deadline100ns + 5000 + i % 2000;
            Check(VioGpuNextVblankDeadline(sample, clock, delay), "advance delayed grid");
            Check(clock.Deadline100ns == start + (i + 1) * period, "no cumulative drift");
            Check(delay > 0 && delay <= period, "future bounded relative delay");
        }
    }
    Check(VioGpuStartVblankClock(0, 60606, clock), "start 165");
    Check(VioGpuNextVblankDeadline(100, clock, delay) && delay == 60506, "early keeps deadline");
    Check(VioGpuNextVblankDeadline(36000000000ULL, clock, delay), "one hour skip bounded");
    Check(clock.Deadline100ns > 36000000000ULL && clock.Deadline100ns % 60606 == 0, "skip preserves phase");
    clock = {~0ULL - 10, 20};
    Check(!VioGpuNextVblankDeadline(~0ULL - 5, clock, delay), "advance overflow rejected");

    VioGpuDod dod;
    const ULONGLONG start = now100ns;
    const ULONGLONG period = VioGpuTimingPeriod100ns(dod.m_CrtcTiming);
    Check(dod.ArmCrtcVsyncTimer() == 0, "arm");
    auto *original = dod.m_CrtcVsyncTimer;
    auto before = sets;
    Check(dod.ArmCrtcVsyncTimer() == 0 && dod.m_CrtcVsyncTimer == original && sets == before, "idempotent arm");
    dod.work100ns = 2000;
    for (unsigned i = 1; i <= 1650; ++i) {
        Fire(dod, 5000);
        Check(original->due == start + (i + 1) * period, "actual callback preserves cadence");
    }
    auto delivered = deliveries;
    now100ns = original->due - 1;
    original->callback(original, &dod);
    Check(deliveries == delivered, "early callback does not invent vblank");
    Fire(dod, period * 100 + 4000);
    Check(deliveries == delivered + 1, "no catch-up burst");
    Check(original->due > now100ns && (original->due - start) % period == 0, "late frame resumes grid");
    // Callback work itself may take several periods: schedule the next future
    // deadline after it, not an already-expired or full-period relative delay.
    dod.work100ns = period * 3 + 1234;
    Fire(dod, 3000);
    Check(original->due > now100ns && (original->due - start) % period == 0, "long callback skips missed slots");
    Timer foreign{original->callback, &dod};
    before = sets; delivered = deliveries;
    now100ns = original->due + 1;
    foreign.callback(&foreign, &dod);
    Check(sets == before && deliveries == delivered, "foreign timer rejected");
    dod.stopInDelivery = true;
    before = sets;
    Fire(dod, 0);
    Check(sets == before, "stop during delivery prevents rearm");
    // The test's simulated stop only closed admission. Let the actual disarm
    // implementation own object destruction once it is called.
    dod.m_CrtcVsyncTimerArmed = 1;
    dod.DisarmCrtcVsyncTimer();
    Check(original->deleted && !dod.m_CrtcVsyncTimer, "disarm drains and clears timer");
    const auto deleted = deletions;
    dod.DisarmCrtcVsyncTimer();
    Check(deletions == deleted, "idempotent disarm");
    dod.stopInDelivery = false;
    Check(dod.ArmCrtcVsyncTimer() == 0, "restart after teardown");
    Check(dod.m_CrtcVsyncTimer != original, "new timer identity");
    before = sets; delivered = deliveries;
    original->callback(original, &dod);
    Check(sets == before && deliveries == delivered, "stale callback rejected after restart");
    dod.DisarmCrtcVsyncTimer();
    failAllocation = true;
    Check(dod.ArmCrtcVsyncTimer() == STATUS_INSUFFICIENT_RESOURCES && !dod.m_CrtcVsyncTimerArmed, "allocation failure");
    failAllocation = false;
    dod.m_CrtcTiming = {};
    Check(dod.ArmCrtcVsyncTimer() == STATUS_INVALID_PARAMETER && !dod.m_CrtcVsyncTimerArmed && !dod.m_CrtcVsyncTimer, "invalid mode cleaned");
    std::printf("vblank clock: %u failures\n", failures);
    return failures ? 1 : 0;
}
