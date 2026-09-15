/* SPDX-License-Identifier: BSD-3-Clause
 * Executes the production IsrDpc.c with a minimal, explicitly mocked WDF API.
 * This is a logic regression test, NOT an ARM64 WDK build or Driver Verifier run.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define IN
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define VOID void
#define BOOLEAN int
#define TRUE 1
#define FALSE 0
#define NTSTATUS int
#define STATUS_SUCCESS 0
#define NT_SUCCESS(s) ((s) >= 0)
#define ULONG unsigned long
#define LONG int
#define PVOID void *
#define UINT unsigned int
#define WDFOBJECT void *
#define TraceEvents(...) ((void)0)
#define WDF_INTERRUPT_INFO_INIT(p) memset((p), 0, sizeof(*(p)))

typedef struct Request { int completed; int reenter; } *WDFREQUEST;
typedef struct virtio_input_event { uint16_t type, code; uint32_t value; } VIRTIO_INPUT_EVENT, *PVIRTIO_INPUT_EVENT;
typedef struct { VIRTIO_INPUT_EVENT Event; WDFREQUEST Request; } VIRTIO_INPUT_EVENT_WITH_REQUEST, *PVIRTIO_INPUT_EVENT_WITH_REQUEST;
struct virtqueue { void *buf[32]; unsigned int len[32]; unsigned int pos, count, kicks, enabled; };
typedef struct Pool { void (*return_slice)(struct Pool *, void *); unsigned int returned; } Pool;
typedef struct Device
{
    struct { int VIODevice; int irq; } VDevice;
    struct virtqueue *EventQ, *StatusQ;
    struct Device *QueuesInterrupt;
    volatile LONG QueuesRunning;
    int *EventQLock, *StatusQLock;
    Pool *EventQMemBlock, *StatusQMemBlock;
} INPUT_DEVICE, *PINPUT_DEVICE, *WDFDEVICE, *WDFINTERRUPT;
typedef struct { int MessageSignaled; } WDF_INTERRUPT_INFO;
static INPUT_DEVICE dev;
static int eventLock, statusLock, messageSignaled, dpcs;
static unsigned int parsed, posted, failPost;
static struct virtqueue eq, sq;
static Pool ep, sp;
static struct Request nestedRequest;
static VIRTIO_INPUT_EVENT_WITH_REQUEST nestedEvent;

// Enqueue a fake used completion; ownership remains observable to the tests.
static void push(struct virtqueue *q, void *p, unsigned int len)
{
    assert(q->count < 32);
    q->buf[q->count] = p;
    q->len[q->count++] = len;
}

// Model dequeue of one used buffer, not submission of new guest work.
static void *virtqueue_get_buf(struct virtqueue *q, unsigned int *len)
{
    assert(q);
    if (q->pos == q->count) return NULL;
    *len = q->len[q->pos];
    return q->buf[q->pos++];
}
static int virtqueue_enable_cb(struct virtqueue *q) { q->enabled = 1; return q->pos == q->count; }
static void virtqueue_disable_cb(struct virtqueue *q) { q->enabled = 0; }
static void virtqueue_kick(struct virtqueue *q) { ++q->kicks; }
static void WdfSpinLockAcquire(int *p) { assert(!*p); *p = 1; }
static void WdfSpinLockRelease(int *p) { assert(*p); *p = 0; }
static WDFDEVICE WdfInterruptGetDevice(WDFINTERRUPT p) { return p; }
static PINPUT_DEVICE GetDeviceContext(WDFDEVICE p) { return p; }
static void WdfInterruptGetInfo(WDFINTERRUPT p, WDF_INTERRUPT_INFO *i) { (void)p; i->MessageSignaled = messageSignaled; }
#define VirtIOWdfGetISRStatus(p) ((p)->irq)
static void WdfInterruptQueueDpcForIsr(WDFINTERRUPT p) { (void)p; ++dpcs; }
static uintptr_t VirtIOWdfDeviceGetPhysicalAddress(int *p, void *b) { (void)p; return (uintptr_t)b; }

// Record recycling and poison the old request cookie after DMA ownership ends.
static void release_slice(Pool *pool, void *p)
{
    assert(p);
    ++pool->returned;
    if (pool == &sp)
    {
        assert(statusLock);
        ((PVIRTIO_INPUT_EVENT_WITH_REQUEST)p)->Request = NULL;
    }
}

// Upper-layer completion is deliberately reentrant to detect spinlock completion.
static void WdfRequestComplete(WDFREQUEST r, int status)
{
    assert(!eventLock && !statusLock);
    assert(status == STATUS_SUCCESS);
    assert(!r->completed);
    r->completed = 1;
    if (r->reenter)
    {
        WdfSpinLockAcquire(&statusLock);
        nestedEvent.Request = &nestedRequest;
        push(&sq, &nestedEvent, 0);
        WdfSpinLockRelease(&statusLock);
    }
}
static void ProcessInputEvent(PINPUT_DEVICE p, PVIRTIO_INPUT_EVENT e) { (void)p; assert(e); ++parsed; }
static NTSTATUS VIOInputAddInBuf(struct virtqueue *q, PVIRTIO_INPUT_EVENT e, uintptr_t pa)
{
    assert(q && e && pa);
    ++posted;
    return failPost ? -1 : 0;
}

// Only the haptic-cookie discriminator is mocked here; test_transport.c covers its implementation.
static BOOLEAN VIOInputHapticsCompleteLocked(PINPUT_DEVICE p, PVOID cookie, WDFREQUEST *r)
{
    (void)p; (void)cookie; (void)r;
    return FALSE;
}
static LONG InterlockedCompareExchange(volatile LONG *p, LONG value, LONG expected)
{
    LONG old = *p;
    if (old == expected) *p = value;
    return old;
}
#include "IsrDpc.c"

// Reset every mock so the test cases cannot depend on execution order.
static void setup(void)
{
    memset(&dev, 0, sizeof(dev)); memset(&eq, 0, sizeof(eq)); memset(&sq, 0, sizeof(sq));
    memset(&nestedRequest, 0, sizeof(nestedRequest));
    ep.return_slice = sp.return_slice = release_slice;
    ep.returned = sp.returned = 0;
    dev.EventQ = &eq; dev.StatusQ = &sq;
    dev.QueuesRunning = 1; dev.QueuesInterrupt = &dev;
    dev.EventQLock = &eventLock; dev.StatusQLock = &statusLock;
    dev.EventQMemBlock = &ep; dev.StatusQMemBlock = &sp;
    eventLock = statusLock = messageSignaled = dpcs = 0;
    parsed = posted = failPost = 0;
}

// Execute the modified driver source and verify malformed input and reentrancy.
int main(void)
{
    VIRTIO_INPUT_EVENT input[4] = {{0}};
    struct Request r = {0, 1};
    VIRTIO_INPUT_EVENT_WITH_REQUEST out = {{0}, &r};
    VIRTIO_INPUT_EVENT_WITH_REQUEST noRequest = {{0}, NULL};
    setup();
    assert(!VIOInputInterruptIsr(&dev, 0) && !dpcs);
    dev.VDevice.irq = 1;
    assert(VIOInputInterruptIsr(&dev, 0) && dpcs == 1);
    dev.VDevice.irq = 0; messageSignaled = 1;
    assert(VIOInputInterruptIsr(&dev, 1) && dpcs == 2);
    setup();
    VIOInputInterruptEnable(&dev, &dev);
    assert(eq.enabled && sq.enabled && eq.kicks == 1 && sq.kicks == 1);
    VIOInputInterruptDisable(&dev, &dev);
    assert(!eq.enabled && !sq.enabled);
    setup();
    push(&eq, &input[0], 8); push(&eq, &input[1], 0);
    push(&eq, &input[2], 7); push(&eq, &input[3], 9);
    push(&sq, &out, 0); push(&sq, &noRequest, 8);
    VIOInputQueuesInterruptDpc(&dev, NULL);
    assert(parsed == 1 && posted == 4 && !ep.returned);
    assert(r.completed && nestedRequest.completed && sp.returned == 3);
    assert(!eventLock && !statusLock);
    VIOInputQueuesInterruptDpc(&dev, NULL);
    assert(sp.returned == 3); /* empty DPC must not double-complete */
    setup();
    failPost = 1; push(&eq, &input[0], 8);
    VIOInputQueuesInterruptDpc(&dev, NULL);
    assert(parsed == 1 && posted == 1 && ep.returned == 1);
    setup();
    dev.EventQ = NULL; dev.StatusQ = NULL;
    VIOInputQueuesInterruptDpc(&dev, NULL);
    VIOInputInterruptEnable(&dev, &dev);
    VIOInputInterruptDisable(&dev, &dev);
    assert(!eventLock && !statusLock);
    puts("production ISR/DPC: shared IRQ, MSI, callback control, invalid lengths,");
    puts("repost failure, reentrant completion, null requests and empty/null queues passed");
    return 0;
}
