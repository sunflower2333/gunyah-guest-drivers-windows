#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>
#include <map>
#include <string>
#ifdef _MSC_VER
#include <intrin.h>
#else
#define __declspec(value) __attribute__((value))
#endif
using ULONG = uint32_t;
using UINT = uint32_t;
using LONG = int32_t;
using NTSTATUS = int32_t;
using LONG64 = int64_t;
using ULONGLONG = uint64_t;
using ULONG_PTR = uintptr_t;
using SIZE_T = size_t;
using UCHAR = uint8_t;
using CHAR = int8_t;
using u32 = uint32_t;
using BOOLEAN = bool;
using PBOOLEAN = BOOLEAN*;
struct LIST_ENTRY { void* next; void* previous; };
struct KEVENT { bool signaled; };
using PKEVENT = KEVENT*;
struct LARGE_INTEGER { LONG64 QuadPart; };
constexpr BOOLEAN TRUE = true, FALSE = false;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_TIMEOUT = 0x102;
constexpr ULONG MAXULONG = UINT32_MAX;
constexpr int Executive = 0, KernelMode = 0, PASSIVE_LEVEL = 0;
#ifndef _Out_
#define _Out_
#endif
#ifndef _In_
#define _In_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#define PAGED_CODE() ((void)0)
#define DbgPrint(...) ((void)0)
#define NT_ASSERT(value) do { if (!(value)) std::abort(); } while (false)
#define FIELD_OFFSET(type, field) offsetof(type, field)
#define RtlCopyMemory(dst, src, count) std::memcpy(dst, src, count)
#define RtlZeroMemory(dst, count) std::memset(dst, 0, count)
UCHAR __ImageBase;
static void* testReturnAddress() { return reinterpret_cast<void*>(reinterpret_cast<ULONG_PTR>(&__ImageBase) + 0x4321); }
#define _ReturnAddress() testReturnAddress()
#include "viogpu_3d_wire.h"
// INSERT_DEFINITIONS

static int checks = 0;
static void check(bool condition, const char* name) {
    ++checks;
    if (!condition) { std::printf("FAIL %s\n", name); std::exit(1); }
}
#ifndef _MSC_VER
static LONG InterlockedCompareExchange(volatile LONG* dest, LONG value, LONG expected) {
    __atomic_compare_exchange_n(dest, &expected, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
static LONG InterlockedExchange(volatile LONG* dest, LONG value) { return __atomic_exchange_n(dest,value,__ATOMIC_SEQ_CST); }
static LONG64 InterlockedCompareExchange64(volatile LONG64* dest, LONG64 value, LONG64 expected) {
    __atomic_compare_exchange_n(dest, &expected, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
#else
static LONG FixtureCompareExchange(volatile LONG* dest, LONG value, LONG expected) {
    return static_cast<LONG>(_InterlockedCompareExchange(reinterpret_cast<volatile long*>(dest), value, expected));
}
static LONG FixtureExchange(volatile LONG* dest, LONG value) {
    return static_cast<LONG>(_InterlockedExchange(reinterpret_cast<volatile long*>(dest), value));
}
#define InterlockedCompareExchange FixtureCompareExchange
#define InterlockedExchange FixtureExchange
#define InterlockedCompareExchange64 _InterlockedCompareExchange64
#endif
static int fixtureIrql = PASSIVE_LEVEL;
static int KeGetCurrentIrql() { return fixtureIrql; }
static void KeClearEvent(KEVENT* event) { event->signaled = false; }
static void KeReleaseMutex(int*, bool) {}
static void NotifyEventCompleteCB(void* ctx) { static_cast<KEVENT*>(ctx)->signaled = true; }
static NTSTATUS waitResult = STATUS_SUCCESS;
static unsigned waits = 0;
static std::function<void()> waitAction;
static NTSTATUS KeWaitForSingleObject(void*, int, int, bool, LARGE_INTEGER* timeout) {
    check(timeout->QuadPart == -50000000LL, "five second wait unchanged");
    ++waits;
    if (waitAction) waitAction();
    return waitResult;
}

struct CtrlQueue {
    volatile LONG64 m_SynchronousEpochState = VioGpuSynchronousOffline;
    volatile LONG m_SynchronousPoisonCallerRva = 0;
    volatile LONG m_SynchronousTimeoutPublication = 0;
    VIOGPU_SYNCHRONOUS_TIMEOUT_DIAGNOSTIC m_FirstSynchronousTimeout = {};
    int m_SynchronousMutex = 0;
    int queueResult = 0;
    unsigned queued = 0;
    int QueueBuffer(PGPU_VBUFFER buf) {
        ++queued;
        check(buf->complete_cb == NotifyEventCompleteCB && buf->complete_ctx == &buf->completion_event &&
              !buf->auto_release && !buf->completion_event.signaled, "queue owns explicit completion buffer");
        return queueResult;
    }
    BOOLEAN IsSynchronousRequestsHealthy();
    void PoisonSynchronousRequests();
    void RecordFirstSynchronousTimeout(PGPU_VBUFFER, NTSTATUS, LONG64, ULONG_PTR);
    BOOLEAN GetFirstSynchronousTimeout(VIOGPU_SYNCHRONOUS_TIMEOUT_DIAGNOSTIC*);
    BOOLEAN EnableSynchronousRequests();
    void CompleteSynchronousRequestTeardown();
    BOOLEAN SubmitSynchronousLocked(PGPU_VBUFFER, PBOOLEAN);
    BOOLEAN SubmitSynchronousLocked(PGPU_VBUFFER, PBOOLEAN, PBOOLEAN);
    ULONG SynchronousPoisonCallerRva(void);
    ULONG SynchronousEpochStateValue(void);
    ULONG SynchronousEpochGenerationValue(void);
    VIOGPU_HOST_CONTEXT_RESULT FlushResourceSynchronous(UINT, UINT, UINT, UINT, UINT);
};
// INSERT_PRODUCTION

static void enable(CtrlQueue& queue) {
    waitAction = {};
    waitResult = STATUS_SUCCESS;
    check(queue.EnableSynchronousRequests() && queue.IsSynchronousRequestsHealthy(), "enable real queue epoch");
}
static void lifecycle() {
    CtrlQueue queue;
    VIOGPU_SYNCHRONOUS_TIMEOUT_DIAGNOSTIC diagnostic;
    GPU_RES_UNREF command = {};
    command.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    command.hdr.ctx_id = 91;
    command.resource_id = 0x80000123;
    GPU_VBUFFER buf = {};
    buf.buf = reinterpret_cast<char*>(&command);
    buf.size = sizeof(command);
    BOOLEAN release = false, submitted = true;
    check(!queue.SubmitSynchronousLocked(&buf, &release, &submitted) && release && !submitted && !queue.queued,
          "offline refuses before queue");
    check(!queue.GetFirstSynchronousTimeout(&diagnostic) && !diagnostic.Flags, "offline has no timeout");
    check(!queue.GetFirstSynchronousTimeout(nullptr), "null reader refused");
    enable(queue);
    queue.queueResult = -1;
    unsigned beforeWaits = waits;
    check(!queue.SubmitSynchronousLocked(&buf,&release,&submitted) && release && !submitted && waits == beforeWaits,
          "queue rejection never waits");
    check(!buf.complete_cb && !buf.complete_ctx && !buf.synchronous_epoch_state &&
          !queue.GetFirstSynchronousTimeout(&diagnostic), "rejected descriptor cleans callback without timeout");
    queue.queueResult = 0;
    waitAction = [&] { buf.complete_cb(buf.complete_ctx); };
    check(queue.SubmitSynchronousLocked(&buf,&release,&submitted) && release && submitted && buf.completion_event.signaled,
          "completion succeeds and releases normally");
    check(!queue.GetFirstSynchronousTimeout(&diagnostic), "success records no timeout");
    waitAction = [&] { buf.synchronous_epoch_state = 0; };
    check(!queue.SubmitSynchronousLocked(&buf,&release,&submitted) && !release && submitted &&
          !queue.GetFirstSynchronousTimeout(&diagnostic), "completion epoch mismatch quarantines without timeout");
    waitAction = {};
    waitResult = STATUS_TIMEOUT;
    check(!queue.SubmitSynchronousLocked(&buf,&release,&submitted) && !release && submitted &&
          !buf.auto_release && buf.complete_cb && buf.complete_ctx, "submitted timeout quarantines owned descriptor");
    check(!queue.IsSynchronousRequestsHealthy(), "timeout poisons actual queue");
    check(queue.GetFirstSynchronousTimeout(&diagnostic) && diagnostic.Flags == 3 &&
          diagnostic.Type == VIRTIO_GPU_CMD_RESOURCE_UNREF && diagnostic.ContextId == 91 &&
          diagnostic.ResourceId == 0x80000123 && diagnostic.WaitStatus == STATUS_TIMEOUT &&
          diagnostic.CallerRva == 0x4321 && diagnostic.CommandBytes == sizeof(command) &&
          diagnostic.EpochGeneration == 1, "timeout captures command and prepoison submission epoch");
    auto first = diagnostic;
    queue.CompleteSynchronousRequestTeardown();
    enable(queue);
    command.hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    command.hdr.ctx_id = 99;
    command.resource_id = 0x123;
    waitResult = static_cast<NTSTATUS>(0xc0000001U);
    check(!queue.SubmitSynchronousLocked(&buf,&release,&submitted) && !release && submitted,
          "second failed submitted wait keeps quarantine");
    check(queue.GetFirstSynchronousTimeout(&diagnostic) && !std::memcmp(&diagnostic,&first,sizeof(first)),
          "first submitted failure retained after recovery");
    beforeWaits = waits;
    check(!queue.SubmitSynchronousLocked(&buf,&release,&submitted) && release && !submitted && waits == beforeWaits,
          "later poisoned refusal cannot overwrite first failure");
    CtrlQueue mutexQueue;
    check(!mutexQueue.EnableSynchronousRequests() && !mutexQueue.GetFirstSynchronousTimeout(&diagnostic),
          "mutex wait failure is not submitted timeout");
    waitResult = STATUS_SUCCESS;
}

template<typename Wire>
static void resource(ULONG type) {
    Wire command = {};
    command.hdr.type = type;
    command.hdr.ctx_id = 7;
    command.resource_id = 0x12345678;
    for (size_t size = 0; size <= sizeof(Wire); ++size) {
        // Exact-sized, deliberately unaligned allocation: ASan rejects overreads.
        std::vector<char> bytes(size + 1);
        std::memcpy(bytes.data() + 1, &command, size);
        GPU_VBUFFER buf = {};
        buf.buf = bytes.data() + 1;
        buf.size = static_cast<int>(size);
        VIOGPU_SYNCHRONOUS_TIMEOUT_DIAGNOSTIC diagnostic = {};
        VioGpuDecodeSynchronousTimeoutCommand(&buf, &diagnostic);
        check(diagnostic.Flags == (size < sizeof(GPU_CTRL_HDR) ? 0U : size < sizeof(Wire) ? 1U : 3U),
              "all truncated fixed command boundaries preserve validity");
        check(diagnostic.ResourceId == (size == sizeof(Wire) ? 0x12345678U : 0U),
              "only full known resource command has resource identity");
    }
}
static void decoding() {
    resource<GPU_RES_CREATE_2D>(VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
    resource<GPU_RES_UNREF>(VIRTIO_GPU_CMD_RESOURCE_UNREF);
    resource<GPU_SET_SCANOUT>(VIRTIO_GPU_CMD_SET_SCANOUT);
    resource<GPU_RES_FLUSH>(VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    resource<GPU_RES_TRANSF_TO_HOST_2D>(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    resource<GPU_RES_ATTACH_BACKING>(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    resource<GPU_RES_DETACH_BACKING>(VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING);
    resource<GPU_CMD_RESOURCE_CREATE_BLOB>(VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB);
    resource<GPU_CMD_SET_SCANOUT_BLOB>(VIRTIO_GPU_CMD_SET_SCANOUT_BLOB);
    resource<GPU_CMD_RESOURCE_MAP_BLOB>(VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB);
    resource<GPU_CMD_RESOURCE_UNMAP_BLOB>(VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB);
    GPU_CMD_SUBMIT_3D command = {};
    command.hdr.ctx_id = 55;
    command.size = 0xfeed;
    GPU_VBUFFER buf = {};
    buf.buf = reinterpret_cast<char*>(&command);
    buf.size = sizeof(command);
    for (ULONG type : {0xffffU, static_cast<ULONG>(VIRTIO_GPU_CMD_SUBMIT_3D),
                       static_cast<ULONG>(VIRTIO_GPU_CMD_CTX_DESTROY)}) {
        command.hdr.type = type;
        VIOGPU_SYNCHRONOUS_TIMEOUT_DIAGNOSTIC diagnostic = {};
        VioGpuDecodeSynchronousTimeoutCommand(&buf,&diagnostic);
        check(diagnostic.Flags == 1 && diagnostic.Type == type && diagnostic.ContextId == 55 && !diagnostic.ResourceId,
              "unknown and nonresource payload never guessed as resource id");
    }
    for (int size : {-1, INT32_MIN, 0, 23}) {
        buf.size = size;
        VIOGPU_SYNCHRONOUS_TIMEOUT_DIAGNOSTIC diagnostic = {};
        VioGpuDecodeSynchronousTimeoutCommand(&buf,&diagnostic);
        check(!diagnostic.Flags, "invalid signed command size is not decoded");
    }
    VIOGPU_SYNCHRONOUS_TIMEOUT_DIAGNOSTIC diagnostic = {};
    buf.buf = nullptr;
    buf.size = 100;
    VioGpuDecodeSynchronousTimeoutCommand(&buf,&diagnostic);
    VioGpuDecodeSynchronousTimeoutCommand(nullptr,&diagnostic);
    check(!diagnostic.Flags, "null buffer/header ignored");
}
static void publication() {
    CtrlQueue queue;
    queue.m_SynchronousTimeoutPublication = 1;
    queue.m_FirstSynchronousTimeout.Flags = 3;
    VIOGPU_SYNCHRONOUS_TIMEOUT_DIAGNOSTIC diagnostic = {};
    check(!queue.GetFirstSynchronousTimeout(&diagnostic) && !diagnostic.Flags, "reader rejects partial publication");
    queue.m_SynchronousTimeoutPublication = 0;
    queue.m_FirstSynchronousTimeout = {};
    GPU_RES_UNREF command = {};
    command.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    command.resource_id = 19;
    GPU_VBUFFER buf = {};
    buf.buf = reinterpret_cast<char*>(&command);
    buf.size = sizeof(command);
    std::atomic<bool> done{false};
    std::atomic<bool> consistent{true};
    std::atomic<bool> started{false};
    std::atomic<unsigned> observations{0};
    std::thread reader([&] {
        started = true;
        while (!done.load()) {
            VIOGPU_SYNCHRONOUS_TIMEOUT_DIAGNOSTIC read = {};
            if (queue.GetFirstSynchronousTimeout(&read)) {
                if (read.Flags != 3 || read.ResourceId != 19 || read.WaitStatus != STATUS_TIMEOUT || read.EpochGeneration != 8)
                    consistent = false;
                ++observations;
            }
        }
    });
    while (!started.load()) std::this_thread::yield();
    queue.RecordFirstSynchronousTimeout(&buf,STATUS_TIMEOUT,VioGpuMakeSynchronousEpochState(8,VioGpuSynchronousEnabled),0);
    for (int i = 0; i < 1000; ++i)
        queue.RecordFirstSynchronousTimeout(&buf,-1,VioGpuMakeSynchronousEpochState(9,VioGpuSynchronousEnabled),0);
    while (observations.load() < 100) std::this_thread::yield();
    done = true;
    reader.join();
    check(consistent && queue.GetFirstSynchronousTimeout(&diagnostic) && diagnostic.EpochGeneration == 8,
          "published first record remains immutable for concurrent readers");
}
// INSERT_MODE_FIXTURE
int main() {
    lifecycle();
    decoding();
    publication();
    mode_publication();
    std::printf("PASS synchronous timeout lifecycle and boundaries: %d checks\n",checks);
}
