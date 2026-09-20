#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
using VOID = void;
using UINT = uint32_t;
using INT32 = int32_t;
using LONG = int32_t;
using ULONGLONG = uint64_t;
using BOOLEAN = bool;
using KIRQL = unsigned;
using NTSTATUS = int;
constexpr bool TRUE = true, FALSE = false;
constexpr int STATUS_SUCCESS = 0, STATUS_INSUFFICIENT_RESOURCES = -1;
constexpr int STATUS_DEVICE_NOT_READY = -2, STATUS_INVALID_PARAMETER = -3, STATUS_INVALID_HANDLE = -4;
constexpr unsigned IO_NO_INCREMENT = 0, PASSIVE_LEVEL = 0;
constexpr UINT VIOGPU_WDDM_CONTEXT_SIGNATURE = 0x1234;
constexpr uint64_t MAXULONGLONG = UINT64_MAX;
struct Event { int signals = 0; int refs = 1; };
using PKEVENT = Event *;
struct VIOGPU_WDDM_CONTEXT {
    UINT Signature = VIOGPU_WDDM_CONTEXT_SIGNATURE;
    std::mutex SubmissionLock;
    bool SubmissionClosing = false, FenceWaitInvalidated = false;
    Event SubmissionProgressEvent;
    UINT UmdFenceHead = 0, UmdFenceCount = 0, UmdFences[4] = {};
    LONG SubmittedUmdFence = 0, CompletedUmdFence = 0;
    struct Waiter { PKEVENT Event; UINT Fence; ULONGLONG Cookie; } FenceWaiters[32] = {};
    ULONGLONG NextFenceWaitCookie = 0;
};
void check(bool value) { if (!value) { std::cout << "FAIL\n"; std::exit(1); } }
void KeAcquireSpinLock(std::mutex *m, KIRQL *level) { *level = 0; m->lock(); }
void KeReleaseSpinLock(std::mutex *m, KIRQL) { m->unlock(); }
void KeSetEvent(PKEVENT e, unsigned, bool) { check(e->refs == 1); ++e->signals; }
void ObDereferenceObjectDeferDelete(PKEVENT e) { check(e->refs == 1); --e->refs; }
void KeClearEvent(PKEVENT e) { e->signals = 0; }
unsigned KeGetCurrentIrql() { return PASSIVE_LEVEL; }
void InterlockedExchange(LONG *p, LONG v) { *p = v; }
void RtlZeroMemory(void *p, size_t n) { std::memset(p, 0, n); }
// PRODUCTION
int main() {
    {
        VIOGPU_WDDM_CONTEXT c;
        Event e;
        ULONGLONG cookie;
        PublishContextCompletedUmdFence(&c, 42);
        check(ArmContextFenceEvent(&c, &e, 42, &cookie) == 0 && cookie == 0);
        check(e.signals == 1 && e.refs == 0);
        PublishContextCompletedUmdFence(&c, 41);
        check(c.CompletedUmdFence == 42);
    }
    {
        VIOGPU_WDDM_CONTEXT c;
        Event events[33]; ULONGLONG cookies[33];
        for (unsigned i = 0; i < 32; ++i)
            check(ArmContextFenceEvent(&c, &events[i], 7 + i, &cookies[i]) == 0);
        check(ArmContextFenceEvent(&c, &events[32], 100, &cookies[32]) == STATUS_INSUFFICIENT_RESOURCES);
        check(events[32].refs == 1);
        PublishContextCompletedUmdFence(&c, 22);
        for (unsigned i = 0; i < 32; ++i)
            check(events[i].signals == (i < 16) && events[i].refs == (i >= 16));
        check(ArmContextFenceEvent(&c, &events[32], 100, &cookies[32]) == 0);
        CancelContextFenceEvent(&c, cookies[0]); // must not cancel reused slot
        check(events[32].refs == 1);
        CancelContextFenceEvent(&c, cookies[32]);
        CancelContextFenceEvent(&c, cookies[32]);
        check(events[32].refs == 0 && events[32].signals == 0);
        InvalidateContextUmdFenceTracker(&c);
        check(c.CompletedUmdFence == 22);
        for (unsigned i = 0; i < 32; ++i) check(events[i].signals == 1 && events[i].refs == 0);
        Event after; ULONGLONG cookie;
        check(ArmContextFenceEvent(&c, &after, 120, &cookie) == STATUS_DEVICE_NOT_READY && after.refs == 1);
    }
    {
        VIOGPU_WDDM_CONTEXT c;
        c.CompletedUmdFence = static_cast<LONG>(0xfffffffeU);
        Event e; ULONGLONG cookie;
        check(ArmContextFenceEvent(&c, &e, 1, &cookie) == 0 && e.signals == 0);
        PublishContextCompletedUmdFence(&c, 1);
        check(e.signals == 1 && e.refs == 0);
        c.NextFenceWaitCookie = MAXULONGLONG;
        Event exhausted;
        check(ArmContextFenceEvent(&c, &exhausted, 2, &cookie) == STATUS_INSUFFICIENT_RESOURCES);
        check(exhausted.refs == 1);
    }
    for (unsigned iteration = 0; iteration < 200; ++iteration) {
        VIOGPU_WDDM_CONTEXT c;
        Event e; ULONGLONG cookie;
        std::thread publisher([&] { PublishContextCompletedUmdFence(&c, 7); });
        check(ArmContextFenceEvent(&c, &e, 7, &cookie) == 0);
        publisher.join();
        check(e.signals == 1 && e.refs == 0);
        Event next;
        check(ArmContextFenceEvent(&c, &next, 8, &cookie) == 0);
        std::thread cancel([&] { CancelContextFenceEvent(&c, cookie); });
        std::thread complete([&] { PublishContextCompletedUmdFence(&c, 8); });
        cancel.join(); complete.join();
        check(next.refs == 0 && next.signals <= 1);
        Event closing;
        check(ArmContextFenceEvent(&c, &closing, 9, &cookie) == 0);
        check(BeginContextSubmissionRundown(&c) == 0);
        check(closing.refs == 0 && closing.signals == 1);
        Event rejected;
        check(ArmContextFenceEvent(&c, &rejected, 10, &cookie) == STATUS_DEVICE_NOT_READY);
        check(rejected.refs == 1);
    }
    std::cout << "PASS: wake/cancel/publication/reset/rundown/wrap/capacity ownership and races\n";
}
