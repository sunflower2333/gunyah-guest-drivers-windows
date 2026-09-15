/* SPDX-License-Identifier: BSD-3-Clause
 * Compile the unmodified production Haptics.c against explicit WDF/VirtIO mocks.
 * These are adversarial ownership/interleaving tests, not a KMDF/WDK substitute.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VOID void
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_PENDING 1
#define STATUS_DEVICE_NOT_READY (-1)
#define STATUS_INVALID_PARAMETER (-2)
#define STATUS_DEVICE_BUSY (-3)
#define STATUS_CANCELLED (-4)
#define STATUS_INSUFFICIENT_RESOURCES (-5)
#define STATUS_DEVICE_CONFIGURATION_ERROR (-6)
#define STATUS_IO_TIMEOUT (-7)
#define NT_SUCCESS(s) ((s) >= 0)
#define NT_ASSERT assert
#define TraceEvents(...) ((void)0)
#define VIOINPUT_DRIVER_MEMORY_TAG 1
#define WDF_REL_TIMEOUT_IN_MS(ms) (ms)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(t,n) static t *n(WDFREQUEST request);
typedef int BOOLEAN, NTSTATUS;
typedef uint8_t UCHAR, *PUCHAR;
typedef uint32_t ULONG;
typedef uint64_t ULONGLONG;
typedef void *PVOID;
typedef struct { uint64_t QuadPart; } PHYSICAL_ADDRESS;
typedef struct Request *WDFREQUEST;
typedef struct _tagInputDevice *PINPUT_DEVICE, *WDFDEVICE;
typedef struct Timer *WDFTIMER;
typedef VOID EVT_WDF_TIMER(WDFTIMER);
typedef VOID EVT_WDF_REQUEST_CANCEL(WDFREQUEST);
#include "Haptics.h"

typedef struct Request
{
    VIOINPUT_HAPTICS_REQUEST Context;
    EVT_WDF_REQUEST_CANCEL *Cancel;
    int Marked, CancelRace, AlreadyCancelled, Completed;
    NTSTATUS Status;
    size_t Information;
} Request;
typedef struct virtqueue
{
    void *Cookie[16];
    unsigned int Count, Kicks;
    int FailAdd;
} Queue;
typedef struct Timer { PINPUT_DEVICE Parent; int Started; } Timer;
typedef struct VirtioMock
{
    int VIODevice;
    uint8_t Cap[DVH_CONFIG_BYTES], CapSize;
    unsigned int Reads;
    int Unstable;
} VirtioMock;
struct _tagInputDevice
{
    VirtioMock VDevice;
    int StatusLock;
    int *StatusQLock;
    int QueuesRunning;
    Queue *StatusQ;
    VIOINPUT_HAPTICS Haptics;
};
struct virtio_input_config
{
    uint8_t select, subsel, size, reserved[5];
    union { uint8_t bitmap[128]; } u;
};
struct VirtIOBufferDescriptor { PHYSICAL_ADDRESS physAddr; ULONG length; };
typedef struct { uint16_t type, code; uint32_t value; } VIRTIO_INPUT_EVENT, *PVIRTIO_INPUT_EVENT;
typedef struct { uint8_t *reportBuffer; ULONG reportBufferLen; uint8_t reportId; } HID_XFER_PACKET, *PHID_XFER_PACKET;
typedef struct { int AutomaticSerialization; EVT_WDF_TIMER *Callback; } WDF_TIMER_CONFIG;
typedef struct { WDFDEVICE ParentObject; } WDF_OBJECT_ATTRIBUTES;

static struct _tagInputDevice dev;
static Queue queue;
static Timer timer;
static uint64_t now;
static unsigned int allocated, freed, sentCount;
static DvhMessage sent[512];

// Mimic device and request context retrieval without hiding request lifetime bugs.
static VIOINPUT_HAPTICS_REQUEST *GetHapticsRequest(WDFREQUEST r) { assert(!r->Completed); return &r->Context; }
static PINPUT_DEVICE GetDeviceContext(WDFDEVICE d) { return d; }
static ULONGLONG KeQueryInterruptTime(void) { return now * 10000; }
static void WdfSpinLockAcquire(int *lock) { assert(!*lock); *lock = 1; }
static void WdfSpinLockRelease(int *lock) { assert(*lock); *lock = 0; }
static size_t RtlCompareMemory(const void *a, const void *b, size_t n) { return memcmp(a,b,n) ? 0 : n; }
static void VirtIOWdfDeviceSet(VirtioMock *d, size_t off, const void *p, size_t n)
{
    (void)d; (void)p; assert(off < 2 && n == 1);
}
static void VirtIOWdfDeviceGet(VirtioMock *d, size_t off, void *p, size_t n)
{
    if (off == offsetof(struct virtio_input_config, size))
    {
        assert(n == 1); *(uint8_t *)p = d->CapSize;
    }
    else
    {
        assert(off == offsetof(struct virtio_input_config, u) && n == DVH_CONFIG_BYTES);
        memcpy(p, d->Cap, n);
        if (d->Unstable && (++d->Reads % 2 == 0)) { ((uint8_t *)p)[28] ^= 1; }
    }
}
static void WDF_TIMER_CONFIG_INIT_PERIODIC(WDF_TIMER_CONFIG *c, EVT_WDF_TIMER *cb, int period)
{
    assert(period == 20); c->Callback = cb;
}
static void WDF_OBJECT_ATTRIBUTES_INIT(WDF_OBJECT_ATTRIBUTES *a) { memset(a,0,sizeof(*a)); }
static NTSTATUS WdfTimerCreate(WDF_TIMER_CONFIG *c, WDF_OBJECT_ATTRIBUTES *a, WDFTIMER *t)
{
    assert(!c->AutomaticSerialization && c->Callback); timer.Parent = a->ParentObject; *t = &timer; return 0;
}
static void WdfTimerStart(WDFTIMER t, int ms) { assert(!dev.StatusLock && ms == 20); t->Started = 1; }
static void WdfTimerStop(WDFTIMER t, int wait) { assert(!dev.StatusLock && wait); t->Started = 0; }
static void *WdfTimerGetParentObject(WDFTIMER t) { return t->Parent; }
static void *VirtIOWdfDeviceAllocDmaMemory(int *d, size_t n, int tag)
{
    (void)d; (void)tag; assert(n == 16 * DVH_FRAME_BYTES && !dev.StatusLock); ++allocated; return calloc(1,n);
}
static PHYSICAL_ADDRESS VirtIOWdfDeviceGetPhysicalAddress(int *d, void *p)
{
    PHYSICAL_ADDRESS pa = {0x100000}; (void)d; assert(p); return pa;
}
static void VirtIOWdfDeviceFreeDmaMemory(int *d, void *p)
{
    (void)d; assert(!dev.StatusLock && !queue.Count); ++freed; free(p);
}
static unsigned int virtio_get_queue_size(Queue *q) { assert(q == &queue); return 16; }
static void virtqueue_kick(Queue *q) { assert(dev.StatusLock); ++q->Kicks; }
static int virtqueue_add_buf(Queue *q, struct VirtIOBufferDescriptor *sg, int out, int in,
                            void *cookie, void *unused, int zero)
{
    unsigned int i;
    uint64_t offset = sg->physAddr.QuadPart - dev.Haptics.WirePa.QuadPart;
    assert(dev.StatusLock && out == 1 && !in && !unused && !zero);
    assert(sg->length == DVH_FRAME_BYTES && offset < 16 * DVH_FRAME_BYTES && offset % DVH_FRAME_BYTES == 0);
    assert(cookie == &dev.Haptics.Slots[offset / DVH_FRAME_BYTES]);
    assert(!dev.Haptics.Slots[offset / DVH_FRAME_BYTES].Busy);
    if (q->FailAdd || q->Count == 16) { return -1; }
    for (i = 0; i < 16; ++i)
    {
        assert(q->Cookie[i] != cookie);
    }
    for (i = 0; q->Cookie[i]; ++i) {}
    q->Cookie[i] = cookie; ++q->Count;
    assert(sentCount < 512 && DvhDecode(dev.Haptics.Wire + offset, DVH_FRAME_BYTES, DVH_EVENT_TYPE, &sent[sentCount]));
    ++sentCount;
    return 0;
}
static NTSTATUS WdfRequestMarkCancelableEx(Request *r, EVT_WDF_REQUEST_CANCEL *cancel)
{
    assert(dev.StatusLock && !r->Marked && !r->Completed);
    if (r->AlreadyCancelled) { return STATUS_CANCELLED; }
    r->Marked = 1; r->Cancel = cancel; return 0;
}
static NTSTATUS WdfRequestUnmarkCancelable(Request *r)
{
    assert(dev.StatusLock && r->Marked && !r->Completed);
    if (r->CancelRace) { return STATUS_CANCELLED; }
    r->Marked = 0; return 0;
}
static void WdfRequestCompleteWithInformation(Request *r, NTSTATUS status, size_t information)
{
    assert(!dev.StatusLock && !r->Completed && !r->Marked);
    r->Completed = 1; r->Status = status; r->Information = information;
}
#include "Haptics.c"

// Supply the exact negotiated capability record, not a forced-on test shortcut.
static void make_caps(uint64_t epoch)
{
    uint8_t *p = dev.VDevice.Cap;
    memset(p,0,DVH_CONFIG_BYTES);
    DvhWrite32(p,UINT32_C(0x31485644)); DvhWrite16(p+4,1); DvhWrite16(p+6,32);
    DvhWrite32(p+8,DVH_CAPS_REQUIRED); DvhWrite16(p+12,DVH_EVENT_TYPE); DvhWrite16(p+14,1);
    DvhWrite32(p+16,(uint32_t)epoch); DvhWrite32(p+20,(uint32_t)(epoch>>32)); DvhWrite32(p+24,500);
    dev.VDevice.CapSize = 32;
}

// Deliver one host control frame as twelve independent real EventQ-sized records.
static void host(uint16_t opcode, uint64_t epoch, uint64_t sequence, uint64_t revision, uint32_t detail)
{
    uint8_t bytes[DVH_FRAME_BYTES]; unsigned int i;
    DvhMessage m = {opcode,epoch,sequence,revision,0,0,detail};
    assert(DvhEncode(bytes,sizeof(bytes),DVH_EVENT_TYPE,&m));
    for (i = 0; i < DVH_RECORDS; ++i)
    {
        VIRTIO_INPUT_EVENT event = {DvhRead16(bytes+i*8),DvhRead16(bytes+i*8+2),DvhRead32(bytes+i*8+4)};
        assert(VIOInputHapticsReceive(&dev,&event));
    }
}

// Return one host-owned cookie; only this helper or simulated reset may free a slot.
static void retire(unsigned int index, NTSTATUS status)
{
    WDFREQUEST r = NULL;
    assert(index < 16 && queue.Cookie[index]);
    WdfSpinLockAcquire(dev.StatusQLock);
    assert(VIOInputHapticsCompleteLocked(&dev,queue.Cookie[index],&r));
    queue.Cookie[index] = NULL; --queue.Count;
    WdfSpinLockRelease(dev.StatusQLock);
    if (r) { WdfRequestCompleteWithInformation(r,status,status == 0 ? 9 : 0); }
}

// Exercise the real startup and READY handshake, never force the state machine to PLAYING.
static void setup(int ready)
{
    assert(allocated == freed);
    memset(&dev,0,sizeof(dev)); memset(&queue,0,sizeof(queue)); memset(&timer,0,sizeof(timer));
    now = 10; sentCount = 0;
    dev.StatusQLock = &dev.StatusLock; dev.StatusQ = &queue; dev.QueuesRunning = 1;
    make_caps(0x100000002ULL);
    dev.Haptics.Enabled = 1;
    assert(VIOInputHapticsInitialize(&dev) == 0);
    assert(VIOInputHapticsAllocate(&dev) == 0);
    assert(VIOInputHapticsStart(&dev) == 0 && timer.Started);
    assert(sentCount == 1 && sent[0].opcode == DVH_GUEST_READY && sent[0].sequence == 1);
    retire(0,0);
    if (ready) { host(DVH_HOST_READY,dev.Haptics.State.epoch,1,0,0); assert(dev.Haptics.State.phase == DVH_IDLE); }
}

// Simulate the documented reset barrier before detaching outstanding DMA.
static void cleanup(void)
{
    unsigned int i;
    VIOInputHapticsQuiesce(&dev);
    dev.QueuesRunning = 0;
    for (i = 0; i < 16; ++i) { if (queue.Cookie[i]) { retire(i,STATUS_DEVICE_NOT_READY); } }
    VIOInputHapticsFree(&dev);
    assert(allocated == freed && !timer.Started);
}

// Send one complete standard report through the actual driver's output entry point.
static NTSTATUS output(Request *r, int stop)
{
    uint8_t bytes[9] = {2,3,0,0,25,50,100,0,0};
    HID_XFER_PACKET packet = {bytes,9,2};
    if (stop) { bytes[4] = bytes[5] = 0; }
    return VIOInputHapticsOutput(&dev,r,&packet);
}

// Deliver an already scheduled cancel callback after it wins cancellation ownership.
static void cancel(Request *r)
{
    assert(r->Marked && !r->Completed && r->Cancel);
    r->Marked = 0; r->Cancel(r);
    assert(r->Completed && r->Status == STATUS_CANCELLED && !r->Information);
}

// Validate old/unstable/malformed capabilities, handshake direction and READY deadline.
static void capabilities(void)
{
    DvhCaps caps; unsigned int i; uint8_t backup[32];
    memset(&dev,0,sizeof(dev)); make_caps(55); memcpy(backup,dev.VDevice.Cap,32);
    for (i = 0; i < 32; ++i) { dev.VDevice.CapSize = (uint8_t)i; assert(!VIOInputHapticsReadCaps(&dev,&caps)); }
    dev.VDevice.CapSize = 32; assert(VIOInputHapticsReadCaps(&dev,&caps) && caps.epoch == 55);
    for (i = 0; i < 16; ++i)
    {
        dev.VDevice.Cap[i] ^= 0x80; assert(!VIOInputHapticsReadCaps(&dev,&caps)); memcpy(dev.VDevice.Cap,backup,32);
    }
    dev.VDevice.Unstable = 1; assert(!VIOInputHapticsReadCaps(&dev,&caps));
    setup(0);
    host(DVH_HOST_READY,777,1,0,0); assert(dev.Haptics.State.phase == DVH_WAIT_READY);
    host(DVH_GUEST_READY,dev.Haptics.State.epoch,1,0,0); assert(dev.Haptics.State.phase == DVH_WAIT_READY);
    now += 2000; host(DVH_HOST_READY,dev.Haptics.State.epoch,1,0,0);
    assert(dev.Haptics.State.phase == DVH_WAIT_READY);
    VIOInputHapticsTimer(&timer); assert(dev.Haptics.State.phase == DVH_REVOKED); cleanup();
}

// A full normal queue cannot consume the two reserved control slots.
static void backpressure(void)
{
    Request requests[16] = {0}; unsigned int i, before;
    setup(1);
    for (i = 0; i < 14; ++i) { assert(output(&requests[i],0) == STATUS_PENDING); }
    assert(dev.Haptics.State.sequence == 15 && dev.Haptics.State.revision == 14);
    assert(output(&requests[14],0) == STATUS_DEVICE_BUSY && !requests[14].Marked);
    assert(dev.Haptics.State.sequence == 15);
    assert(output(&requests[15],1) == STATUS_PENDING && dev.Haptics.State.phase == DVH_IDLE);
    assert(queue.Count == 15 && sent[sentCount-1].opcode == DVH_STOP);
    before = sentCount; now += 125; VIOInputHapticsTimer(&timer); assert(sentCount == before);
    cleanup();
    for (i = 0; i < 14; ++i) { assert(requests[i].Completed); }
    assert(requests[15].Completed && !requests[14].Completed);
}

// Failed publication never commits state; mark/unmark cancellation races have a unique owner.
static void enqueue_failures(void)
{
    Request r = {0}; setup(1); queue.FailAdd = 1;
    assert(output(&r,0) == STATUS_DEVICE_BUSY && !r.Marked && !r.Completed);
    assert(!queue.Count && !dev.Haptics.State.revision);
    r.CancelRace = 1;
    assert(output(&r,0) == STATUS_PENDING && r.Marked && !r.Context.Slot);
    assert(!queue.Count && !dev.Haptics.State.revision); cancel(&r);
    queue.FailAdd = 0; cleanup();
    memset(&r,0,sizeof(r)); setup(1); r.AlreadyCancelled = 1;
    assert(output(&r,0) == STATUS_CANCELLED && !r.Marked && !queue.Count);
    cleanup();
}

// Cancel, timeout and EvtIoStop finish requests but retain immutable host-visible DMA.
static void detached_ownership(void)
{
    Request r = {0}; uint8_t original[DVH_FRAME_BYTES]; unsigned int normal = 2;
    setup(1); assert(output(&r,0) == STATUS_PENDING);
    memcpy(original,dev.Haptics.Wire+normal*DVH_FRAME_BYTES,sizeof(original));
    cancel(&r);
    assert(dev.Haptics.Slots[normal].Busy && dev.Haptics.State.phase == DVH_REVOKED);
    assert(!memcmp(original,dev.Haptics.Wire+normal*DVH_FRAME_BYTES,sizeof(original))); cleanup();
    memset(&r,0,sizeof(r)); setup(1); assert(output(&r,0) == STATUS_PENDING);
    now += 1000; VIOInputHapticsTimer(&timer);
    assert(r.Completed && r.Status == STATUS_IO_TIMEOUT && !r.Information && dev.Haptics.Slots[normal].Busy);
    cleanup();
    memset(&r,0,sizeof(r)); setup(1); assert(output(&r,0) == STATUS_PENDING);
    VIOInputHapticsStopRequest(&dev,&r);
    assert(r.Completed && r.Status == STATUS_DEVICE_NOT_READY && dev.Haptics.Slots[normal].Busy);
    cleanup();
    memset(&r,0,sizeof(r)); setup(1); assert(output(&r,0) == STATUS_PENDING); r.CancelRace = 1;
    retire(0,0); assert(!r.Completed && !dev.Haptics.Slots[normal].Busy && !r.Context.Slot);
    cancel(&r); cleanup();
}

// Exact-revision keepalive cannot replay finished effects or survive a host revocation.
static void leases_and_epochs(void)
{
    Request r[3] = {0}; DvhMessage first; unsigned int before;
    setup(1); assert(output(&r[0],0) == STATUS_PENDING); first = sent[sentCount-1]; retire(0,0);
    now += 125; VIOInputHapticsTimer(&timer);
    assert(sent[sentCount-1].opcode == DVH_KEEPALIVE_XINPUT_REPORT);
    assert(sent[sentCount-1].revision == first.revision && sent[sentCount-1].detail == first.detail);
    retire(0,0);
    host(DVH_HOST_STATUS,first.epoch,2,first.revision,1); assert(dev.Haptics.State.phase == DVH_IDLE);
    before = sentCount; now += 500; VIOInputHapticsTimer(&timer); assert(sentCount == before);
    assert(output(&r[1],0) == STATUS_PENDING);
    host(DVH_HOST_STATUS,first.epoch,3,first.revision,1); assert(dev.Haptics.State.phase == DVH_PLAYING);
    host(DVH_HOST_REVOKE,first.epoch,1,0,0); assert(dev.Haptics.State.phase == DVH_PLAYING); // replay
    host(DVH_HOST_REVOKE,first.epoch,4,0,0); assert(dev.Haptics.State.phase == DVH_REVOKED);
    assert(output(&r[2],0) == STATUS_DEVICE_NOT_READY); cleanup();
    // A D0 restart with the old epoch is rejected, never an implicit resume of motors.
    setup(1); assert(VIOInputHapticsStart(&dev) == STATUS_DEVICE_CONFIGURATION_ERROR); cleanup();
}

// Entry point: assertions deliberately fail on double completion or premature DMA recycling.
int main(void)
{
    capabilities(); backpressure(); enqueue_failures(); detached_ownership(); leases_and_epochs();
    puts("production haptics: caps, READY, backpressure, reserved STOP, cancel/used races,");
    puts("timeout and power-stop DMA retention, leases, finite completion and stale epochs passed");
    return 0;
}
