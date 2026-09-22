// SPDX-License-Identifier: BSD-3-Clause
// Pool methods/record are extracted from production; kernel allocation and callbacks are mocked.
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <unordered_set>
#include <vector>
using UINT = unsigned int;
using u32 = uint32_t;
using LONG = int;
using LONG64 = int64_t;
using ULONGLONG = unsigned long long;
using BOOLEAN = bool;
using VOID = void;
using PVOID = void *;
using SIZE_T = size_t;
using ULONG_PTR = uintptr_t;
using KIRQL = int;
#ifndef _In_
#define _In_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#define TRUE                      true
#define FALSE                     false
#define ASSERT                    assert
#define PAGED_CODE()              ((void)0)
#define DbgPrint(...)             ((void)0)
#define UNREFERENCED_PARAMETER(x) ((void)(x))
constexpr int PAGE_SIZE = 4096, DISPATCH_LEVEL = 2, NotificationEvent = 0, NonPagedPoolNx = 0;
constexpr unsigned VIOGPUTAG = 1;
struct LIST_ENTRY
{
    LIST_ENTRY *Flink, *Blink;
};
using PLIST_ENTRY = LIST_ENTRY *;
#define CONTAINING_RECORD(p, t, f) reinterpret_cast<t *>(reinterpret_cast<char *>(p) - offsetof(t, f))
void InitializeListHead(PLIST_ENTRY p)
{
    p->Flink = p->Blink = p;
}
bool IsListEmpty(PLIST_ENTRY p)
{
    return p->Flink == p;
}
void InsertTailList(PLIST_ENTRY h, PLIST_ENTRY e)
{
    e->Blink = h->Blink;
    e->Flink = h;
    h->Blink->Flink = e;
    h->Blink = e;
}
void RemoveEntryList(PLIST_ENTRY e)
{
    assert(e->Blink->Flink == e && e->Flink->Blink == e);
    e->Blink->Flink = e->Flink;
    e->Flink->Blink = e->Blink;
}
PLIST_ENTRY RemoveHeadList(PLIST_ENTRY h)
{
    auto e = h->Flink;
    RemoveEntryList(e);
    return e;
}
using KSPIN_LOCK = std::mutex;
thread_local KIRQL irql = 0;
void KeInitializeSpinLock(KSPIN_LOCK *)
{
}
void KeAcquireSpinLock(KSPIN_LOCK *m, KIRQL *old)
{
    *old = irql;
    m->lock();
    irql = DISPATCH_LEVEL;
}
void KeReleaseSpinLock(KSPIN_LOCK *m, KIRQL old)
{
    irql = old;
    m->unlock();
}
void KeAcquireSpinLockAtDpcLevel(KSPIN_LOCK *m)
{
    m->lock();
}
void KeReleaseSpinLockFromDpcLevel(KSPIN_LOCK *m)
{
    m->unlock();
}
KIRQL KeGetCurrentIrql()
{
    return irql;
}
struct KEVENT
{
    bool set;
};
void KeInitializeEvent(KEVENT *e, int, bool set)
{
    e->set = set;
}
void RtlZeroMemory(void *p, size_t n)
{
    memset(p, 0, n);
}
std::unordered_set<void *> allocations;
void *ExAllocatePoolUninitialized(int, size_t n, unsigned)
{
    void *p = malloc(n);
    assert(p);
    assert(allocations.insert(p).second);
    return p;
}
void ExFreePoolWithTag(void *p, unsigned)
{
    assert(allocations.erase(p) == 1);
    free(p);
}
// INSERT_RECORDS
#define MAX_INLINE_CMD_SIZE  96
#define MAX_INLINE_RESP_SIZE 24
#define VBUFFER_SIZE         (sizeof(GPU_VBUFFER) + MAX_INLINE_CMD_SIZE + MAX_INLINE_RESP_SIZE)
// INSERT_CLASS
PGPU_VBUFFER held = nullptr;
bool callbackDetachedFree = false;
unsigned cancellations = 0;
VIOGPU_VBUFFER_TERMINAL_CLAIM VioGpuClaimVbufferTerminalCallbacks(PGPU_VBUFFER buffer)
{
    // Reset/close must revoke the O(1) ownership before callbacks run unlocked.
    assert(!buffer->pool_in_use);
    if (callbackDetachedFree)
    {
        static_cast<VioGpuBuf *>(buffer->pool_owner)->FreeBuf(buffer);
    }
    return buffer == held ? VioGpuVbufferTerminalClaimLost : VioGpuVbufferTerminalClaimWon;
}
void VioGpuDetachVbufferTerminalCallbacks(PGPU_VBUFFER p)
{
    p->complete_cb = nullptr;
    p->complete_ctx = nullptr;
    p->cancel_cb = nullptr;
    p->cancel_ctx = nullptr;
    p->queue_error_cb = nullptr;
    p->queue_error_ctx = nullptr;
}
void VioGpuCompleteVbufferTerminalCallbacks(PGPU_VBUFFER)
{
}
BOOLEAN VioGpuWaitForVbufferTerminalCallbacks(PGPU_VBUFFER p)
{
    return p != held;
}
void cancelled(void *)
{
    ++cancellations;
}
// INSERT_PRODUCTION

// Validate both intrusive lists and ownership after every scenario.
void verify(VioGpuBuf &pool, UINT inUse, UINT cached)
{
    auto count = [&](LIST_ENTRY *h, bool live) {
        UINT n = 0;
        for (auto e = h->Flink; e != h; e = e->Flink)
        {
            assert(e->Flink->Blink == e && e->Blink->Flink == e);
            auto p = CONTAINING_RECORD(e, GPU_VBUFFER, list_entry);
            assert(allocations.count(p));
            if (live)
            {
                assert(p->pool_owner == &pool && p->pool_in_use);
            }
            else
            {
                assert(!p->pool_in_use);
            }
            assert(++n <= pool.m_uCount);
        }
        return n;
    };
    assert(count(&pool.m_InUseBufs, true) == inUse);
    assert(count(&pool.m_FreeBufs, false) == cached);
    assert(pool.m_uCount == inUse + cached);
}

int main()
{
    {
        VioGpuBuf pool, other;
        assert(pool.Init(8));
        verify(pool, 0, 8);
        assert(!pool.GetBuf(0, 8, nullptr));
        assert(!pool.GetBuf(8, 25, nullptr));
        std::vector<PGPU_VBUFFER> buffers;
        for (UINT i = 0; i < 2048; ++i)
        {
            buffers.push_back(pool.GetBuf(8, 8, nullptr));
        }
        verify(pool, 2048, 0);
        other.FreeBuf(buffers[10]);
        verify(pool, 2048, 0); // no foreign-list mutation
        std::mt19937 random(91);
        std::shuffle(buffers.begin(), buffers.end(), random);
        for (auto p : buffers)
        {
            pool.FreeBuf(p);
        }
        verify(pool, 0, 8);
        auto p = pool.GetBuf(8, 8, nullptr);
        pool.FreeBuf(p);
        pool.FreeBuf(p); // still-cached duplicate
        verify(pool, 0, 8);
        p = pool.GetBuf(8, 32, static_cast<char *>(pool.AllocateMemory(32)));
        p->data_buf = pool.AllocateMemoryUninitialized(4096);
        p->data_size = 4096;
        pool.FreeBuf(p);
        verify(pool, 0, 8);
        assert(pool.Close());
        verify(pool, 0, 0);
    }
    assert(allocations.empty());
    for (bool close : {false, true})
    {
        VioGpuBuf pool;
        assert(pool.Init(2));
        for (UINT i = 0; i < 17; ++i)
        {
            auto p = pool.GetBuf(8, 8, nullptr);
            p->cancel_cb = cancelled;
            p->data_buf = pool.AllocateMemoryUninitialized(128);
            p->data_size = 128;
            if (i == 3)
            {
                held = p;
            }
        }
        callbackDetachedFree = true;
        cancellations = 0;
        if (close)
        {
            assert(!pool.Close());
        }
        else
        {
            pool.ReclaimBuffers();
        }
        assert(cancellations == 16);
        verify(pool, 1, close ? 0 : 1);
        // Terminal claimant timed out: owner flag must be restored for a later DPC.
        auto pending = held;
        held = nullptr;
        callbackDetachedFree = false;
        pool.FreeBuf(pending);
        verify(pool, 0, close ? 0 : 2);
        assert(pool.Close());
    }
    assert(allocations.empty());
    puts("PASS production VBUFFER pool: 2048 unordered frees, reuse, ownership, detached reset callbacks, timeout "
         "reinsert, external payload cleanup");
}
