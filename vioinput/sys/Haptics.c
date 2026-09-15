/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 DroidVM contributors
 * Private, negotiated XInputHID report transport over the existing StatusQ.
 * All state and slot ownership is protected by StatusQLock. Wire pages contain
 * only bytes, never WDF handles, list links or kernel virtual addresses.
 */
#include "precomp.h"
#include "vioinput.h"
#if defined(EVENT_TRACING)
#include "Haptics.tmh"
#endif

#define HAPTICS_COMPLETION_TIMEOUT_MS 1000u
#define HAPTICS_READY_TIMEOUT_MS 2000u
#define HAPTICS_TICK_MS 20u

static EVT_WDF_TIMER VIOInputHapticsTimer;
static EVT_WDF_REQUEST_CANCEL VIOInputHapticsCancel;

// Use a monotonic clock; guest wall-clock adjustments must not extend leases.
static ULONGLONG HapticsNowMs(VOID)
{
    return KeQueryInterruptTime() / 10000;
}

// Read the complete capability record twice, failing closed on unstable config.
BOOLEAN VIOInputHapticsReadCaps(PINPUT_DEVICE pContext, DvhCaps *Caps)
{
    UCHAR first[DVH_CONFIG_BYTES], second[DVH_CONFIG_BYTES];
    UCHAR select = DVH_CONFIG_SELECT, subsel = DVH_CONFIG_SUBSEL, size = 0;
    ULONG pass;
    for (pass = 0; pass < 2; ++pass)
    {
        VirtIOWdfDeviceSet(&pContext->VDevice, offsetof(struct virtio_input_config, select), &select, 1);
        VirtIOWdfDeviceSet(&pContext->VDevice, offsetof(struct virtio_input_config, subsel), &subsel, 1);
        VirtIOWdfDeviceGet(&pContext->VDevice, offsetof(struct virtio_input_config, size), &size, 1);
        if (size != DVH_CONFIG_BYTES)
        {
            return FALSE;
        }
        VirtIOWdfDeviceGet(&pContext->VDevice, offsetof(struct virtio_input_config, u),
                          pass ? second : first, DVH_CONFIG_BYTES);
    }
    return RtlCompareMemory(first, second, sizeof(first)) == sizeof(first) &&
           DvhParseCaps(first, sizeof(first), Caps);
}

// Allocate the timer once; disable implicit serialization and use our explicit lock.
NTSTATUS VIOInputHapticsInitialize(WDFDEVICE Device)
{
    PINPUT_DEVICE pContext = GetDeviceContext(Device);
    WDF_TIMER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_TIMER_CONFIG_INIT_PERIODIC(&config, VIOInputHapticsTimer, HAPTICS_TICK_MS);
    config.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    return WdfTimerCreate(&config, &attributes, &pContext->Haptics.Timer);
}

// Allocate a dedicated, fixed-size DMA region, without changing the shared DMA pool.
NTSTATUS VIOInputHapticsAllocate(PINPUT_DEVICE pContext)
{
    VIOINPUT_HAPTICS *h = &pContext->Haptics;
    if (!h->Enabled)
    {
        return STATUS_SUCCESS;
    }
    h->Wire = VirtIOWdfDeviceAllocDmaMemory(&pContext->VDevice.VIODevice,
                                           VIOINPUT_HAPTICS_SLOTS * DVH_FRAME_BYTES,
                                           VIOINPUT_DRIVER_MEMORY_TAG);
    if (!h->Wire)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    h->WirePa = VirtIOWdfDeviceGetPhysicalAddress(&pContext->VDevice.VIODevice, h->Wire);
    RtlZeroMemory(h->Wire, VIOINPUT_HAPTICS_SLOTS * DVH_FRAME_BYTES);
    return STATUS_SUCCESS;
}

// Separate WDF ownership from DMA ownership; cancellation never frees a published slot.
static WDFREQUEST HapticsDetachRequestLocked(PVIOINPUT_HAPTICS_SLOT Slot)
{
    WDFREQUEST request = Slot->Request;
    if (request)
    {
        Slot->Request = NULL;
        GetHapticsRequest(request)->Slot = NULL;
        if (WdfRequestUnmarkCancelable(request) == STATUS_CANCELLED)
        {
            return NULL; // the cancel callback is the unique request completion owner
        }
    }
    return request;
}

// Publish one whole 96-byte frame in one descriptor; commit state only when Published is true.
static NTSTATUS HapticsSendLocked(
    PINPUT_DEVICE pContext,
    const DvhMessage *Message,
    WDFREQUEST Request,
    BOOLEAN Control,
    BOOLEAN *Published)
{
    VIOINPUT_HAPTICS *h = &pContext->Haptics;
    PVIOINPUT_HAPTICS_SLOT slot = NULL;
    struct VirtIOBufferDescriptor sg;
    ULONG i;
    NTSTATUS status;
    *Published = FALSE;
    if (!pContext->QueuesRunning || !pContext->StatusQ || !h->Wire || (!Control && h->Closing))
    {
        return STATUS_DEVICE_NOT_READY;
    }
    for (i = Control ? 0 : VIOINPUT_HAPTICS_CONTROL_SLOTS;
         i < (Control ? VIOINPUT_HAPTICS_CONTROL_SLOTS : VIOINPUT_HAPTICS_SLOTS); ++i)
    {
        if (!h->Slots[i].Busy)
        {
            slot = &h->Slots[i];
            break;
        }
    }
    if (!slot)
    {
        return STATUS_DEVICE_BUSY;
    }
    if (!DvhEncode(h->Wire + i * DVH_FRAME_BYTES, DVH_FRAME_BYTES, DVH_EVENT_TYPE, Message))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (Request)
    {
        PVIOINPUT_HAPTICS_REQUEST r = GetHapticsRequest(Request);
        r->Owner = pContext;
        r->Slot = slot;
        r->Epoch = Message->epoch;
        r->Revision = Message->revision;
        status = WdfRequestMarkCancelableEx(Request, VIOInputHapticsCancel);
        if (!NT_SUCCESS(status))
        {
            r->Slot = NULL;
            return status;
        }
    }
    slot->Request = Request;
    sg.physAddr.QuadPart = h->WirePa.QuadPart + i * DVH_FRAME_BYTES;
    sg.length = DVH_FRAME_BYTES;
    if (virtqueue_add_buf(pContext->StatusQ, &sg, 1, 0, slot, NULL, 0) < 0)
    {
        /* Nothing was published. A racing cancel callback still owns its request. */
        if (Request && !HapticsDetachRequestLocked(slot))
        {
            return STATUS_PENDING;
        }
        return STATUS_DEVICE_BUSY;
    }
    slot->Busy = TRUE;
    slot->PublishedMs = HapticsNowMs();
    h->LastSendMs = slot->PublishedMs;
    *Published = TRUE;
    virtqueue_kick(pContext->StatusQ);
    return Request ? STATUS_PENDING : STATUS_SUCCESS;
}

// Retry a sticky stop from reserved control slots; never renew after a stop fails to enqueue.
static VOID HapticsStopLocked(PINPUT_DEVICE pContext)
{
    VIOINPUT_HAPTICS *h = &pContext->Haptics;
    DvhMessage message;
    BOOLEAN published;
    if (!h->StopPending || !h->State.epoch || h->State.sequence == UINT64_MAX)
    {
        return;
    }
    RtlZeroMemory(&message, sizeof(message));
    message.opcode = DVH_CLOSE;
    message.epoch = h->State.epoch;
    message.sequence = h->State.sequence + 1;
    message.revision = h->State.revision;
    HapticsSendLocked(pContext, &message, NULL, TRUE, &published);
    if (published)
    {
        h->State.sequence = message.sequence;
        h->StopPending = FALSE;
    }
}

// Revoke a faulty session without claiming that the host has returned its DMA ownership.
static VOID HapticsFaultLocked(PINPUT_DEVICE pContext)
{
    VIOINPUT_HAPTICS *h = &pContext->Haptics;
    if (h->State.phase != DVH_DISABLED && h->State.phase != DVH_REVOKED)
    {
        h->StopPending = TRUE;
        DvhGuestRevoke(&h->State);
        HapticsStopLocked(pContext);
    }
}

// Begin each D0 epoch from silence and require a fresh host-generated capability epoch.
NTSTATUS VIOInputHapticsStart(PINPUT_DEVICE pContext)
{
    VIOINPUT_HAPTICS *h = &pContext->Haptics;
    DvhCaps caps;
    DvhMessage ready;
    BOOLEAN published;
    NTSTATUS status;
    if (!h->Enabled)
    {
        return STATUS_SUCCESS;
    }
    if (!VIOInputHapticsReadCaps(pContext, &caps) || caps.epoch == h->LastEpoch ||
        virtio_get_queue_size(pContext->StatusQ) < VIOINPUT_HAPTICS_SLOTS)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    WdfSpinLockAcquire(pContext->StatusQLock);
    h->Caps = caps;
    h->LastEpoch = caps.epoch;
    h->Closing = h->StopPending = h->ReadySent = FALSE;
    h->HostSequence = 0;
    DvhReceiverReset(&h->Receiver);
    DvhGuestReset(&h->State);
    DvhGuestBegin(&h->State, caps.epoch);
    RtlZeroMemory(&ready, sizeof(ready));
    ready.opcode = DVH_GUEST_READY;
    ready.epoch = caps.epoch;
    ready.sequence = 1;
    status = HapticsSendLocked(pContext, &ready, NULL, TRUE, &published);
    if (published)
    {
        h->State.sequence = 1; // handshake and output share the guest sequence domain
        h->ReadySent = TRUE;
        h->ReadyDeadlineMs = HapticsNowMs() + HAPTICS_READY_TIMEOUT_MS;
    }
    WdfSpinLockRelease(pContext->StatusQLock);
    if (NT_SUCCESS(status))
    {
        WdfTimerStart(h->Timer, WDF_REL_TIMEOUT_IN_MS(HAPTICS_TICK_MS));
    }
    return status;
}

// Consume only negotiated control records; never feed private frames into keyboard/tablet decoding.
BOOLEAN VIOInputHapticsReceive(PINPUT_DEVICE pContext, PVIRTIO_INPUT_EVENT Event)
{
    VIOINPUT_HAPTICS *h = &pContext->Haptics;
    DvhMessage message;
    UCHAR record[DVH_RECORD_BYTES];
    int result;
    if (!h->Enabled || Event->type != DVH_EVENT_TYPE)
    {
        return FALSE;
    }
    DvhWrite16(record, Event->type);
    DvhWrite16(record + 2, Event->code);
    DvhWrite32(record + 4, Event->value);
    WdfSpinLockAcquire(pContext->StatusQLock);
    result = DvhReceiveRecord(&h->Receiver, record, DVH_EVENT_TYPE, HapticsNowMs(), 100, &message);
    if (result == 1 && !h->Closing && message.epoch == h->State.epoch && message.sequence > h->HostSequence)
    {
        /* Only these three direction-specific opcodes may mutate guest state. */
        if (message.opcode == DVH_HOST_READY && h->ReadySent && h->State.phase == DVH_WAIT_READY &&
            HapticsNowMs() < h->ReadyDeadlineMs)
        {
            h->HostSequence = message.sequence;
            DvhGuestReady(&h->State, message.epoch);
        }
        else if (message.opcode == DVH_HOST_REVOKE)
        {
            h->HostSequence = message.sequence;
            HapticsFaultLocked(pContext);
        }
        else if (message.opcode == DVH_HOST_STATUS && message.revision == h->State.revision)
        {
            h->HostSequence = message.sequence;
            if (message.detail == 1 && h->State.phase == DVH_PLAYING)
            {
                h->State.phase = DVH_IDLE; // finite effect ended; keepalive must not replay it
                h->State.motors = h->State.detail = h->State.lease_ms = 0;
            }
            else if (message.detail >= 2)
            {
                HapticsFaultLocked(pContext);
            }
        }
    }
    WdfSpinLockRelease(pContext->StatusQLock);
    return TRUE;
}

// Decode actual report ID 2 and retain the WDF request only after a successful publish.
NTSTATUS VIOInputHapticsOutput(PINPUT_DEVICE pContext, WDFREQUEST Request, PHID_XFER_PACKET Packet)
{
    VIOINPUT_HAPTICS *h = &pContext->Haptics;
    DvhMotorReport report;
    DvhMessage message;
    BOOLEAN published = FALSE;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    if (!Packet || Packet->reportId != 2 ||
        !DvhParseMotorReport(Packet->reportBuffer, Packet->reportBufferLen, &report))
    {
        return STATUS_INVALID_PARAMETER;
    }
    WdfSpinLockAcquire(pContext->StatusQLock);
    if (h->Enabled && !h->Closing && !h->StopPending &&
        (report.stop ? DvhGuestPrepareOutput(&h->State, 0, 0, 0, &message) :
         DvhGuestPrepareXinputReport(&h->State, report.magnitudes, report.timing, h->Caps.lease_ms, &message)))
    {
        status = HapticsSendLocked(pContext, &message, Request, report.stop ? TRUE : FALSE, &published);
        if (published)
        {
            if (!DvhGuestCommit(&h->State, &message))
            {
                HapticsFaultLocked(pContext);
            }
        }
        else if (report.stop)
        {
            HapticsFaultLocked(pContext);
        }
    }
    WdfSpinLockRelease(pContext->StatusQLock);
    if (published)
    {
        TraceEvents(TRACE_LEVEL_VERBOSE, DBG_WRITE,
                    "XInput output published\nepoch=%I64u\nsequence=%I64u\nrevision=%I64u\nstop=%d\n",
                    message.epoch, message.sequence, message.revision, report.stop);
    }
    return status;
}

// Match private cookies by identity before any legacy-cookie cast; caller holds StatusQLock.
BOOLEAN VIOInputHapticsCompleteLocked(PINPUT_DEVICE pContext, PVOID Cookie, WDFREQUEST *Request)
{
    ULONG i;
    for (i = 0; i < VIOINPUT_HAPTICS_SLOTS; ++i)
    {
        PVIOINPUT_HAPTICS_SLOT slot = &pContext->Haptics.Slots[i];
        if (Cookie == slot)
        {
            *Request = HapticsDetachRequestLocked(slot);
            slot->Busy = FALSE; // called only after used completion or confirmed device reset
            return TRUE;
        }
    }
    return FALSE;
}

// A cancel may detach a request, but only used/reset may release the published DMA slot.
static VOID VIOInputHapticsCancel(WDFREQUEST Request)
{
    PVIOINPUT_HAPTICS_REQUEST r = GetHapticsRequest(Request);
    PINPUT_DEVICE pContext = r->Owner;
    WdfSpinLockAcquire(pContext->StatusQLock);
    if (r->Slot && r->Slot->Request == Request)
    {
        r->Slot->Request = NULL;
    }
    r->Slot = NULL;
    if (r->Epoch == pContext->Haptics.State.epoch && r->Revision == pContext->Haptics.State.revision)
    {
        HapticsFaultLocked(pContext);
    }
    WdfSpinLockRelease(pContext->StatusQLock);
    WdfRequestCompleteWithInformation(Request, STATUS_CANCELLED, 0);
}

// Detach stopping WDF requests now; their private DMA slots remain owned until used/reset.
VOID VIOInputHapticsStopRequest(PINPUT_DEVICE pContext, WDFREQUEST Request)
{
    PVIOINPUT_HAPTICS_REQUEST r = GetHapticsRequest(Request);
    WDFREQUEST complete = NULL;
    WdfSpinLockAcquire(pContext->StatusQLock);
    pContext->Haptics.Closing = TRUE;
    HapticsFaultLocked(pContext);
    if (r->Slot && r->Slot->Request == Request)
    {
        complete = HapticsDetachRequestLocked(r->Slot);
    }
    WdfSpinLockRelease(pContext->StatusQLock);
    if (complete)
    {
        WdfRequestCompleteWithInformation(complete, STATUS_DEVICE_NOT_READY, 0);
    }
    // Otherwise used/timeout/cancel already owns completion; never complete it twice.
}

// Bound pending WDF requests and renew exact revisions, never game/process liveness.
static VOID VIOInputHapticsTimer(WDFTIMER Timer)
{
    PINPUT_DEVICE pContext = GetDeviceContext((WDFDEVICE)WdfTimerGetParentObject(Timer));
    VIOINPUT_HAPTICS *h = &pContext->Haptics;
    WDFREQUEST expired[VIOINPUT_HAPTICS_SLOTS];
    ULONG count = 0, i;
    ULONGLONG now = HapticsNowMs();
    BOOLEAN fault = FALSE, published;
    DvhMessage message;
    WdfSpinLockAcquire(pContext->StatusQLock);
    if (!h->Closing && pContext->QueuesRunning)
    {
        for (i = 0; i < VIOINPUT_HAPTICS_SLOTS; ++i)
        {
            PVIOINPUT_HAPTICS_SLOT slot = &h->Slots[i];
            if (slot->Busy && now - slot->PublishedMs >= HAPTICS_COMPLETION_TIMEOUT_MS)
            {
                WDFREQUEST request = HapticsDetachRequestLocked(slot);
                if (request)
                {
                    expired[count++] = request;
                }
                fault = TRUE;
            }
        }
        if (fault || (h->State.phase == DVH_WAIT_READY && now >= h->ReadyDeadlineMs))
        {
            HapticsFaultLocked(pContext);
        }
        HapticsStopLocked(pContext);
        if (!h->StopPending && now - h->LastSendMs >= h->Caps.lease_ms / 4 &&
            DvhGuestPrepareKeepalive(&h->State, &message))
        {
            HapticsSendLocked(pContext, &message, NULL, FALSE, &published);
            if (published)
            {
                DvhGuestCommit(&h->State, &message);
            }
        }
    }
    WdfSpinLockRelease(pContext->StatusQLock);
    for (i = 0; i < count; ++i)
    {
        WdfRequestCompleteWithInformation(expired[i], STATUS_IO_TIMEOUT, 0);
    }
    if (count)
    {
        TraceEvents(TRACE_LEVEL_WARNING, DBG_WRITE, "Haptics transport timeout\nrequests=%lu\nDMA_retained=1\n", count);
    }
}

// Close producers first and synchronously join the timer, outside every driver lock.
VOID VIOInputHapticsQuiesce(PINPUT_DEVICE pContext)
{
    VIOINPUT_HAPTICS *h = &pContext->Haptics;
    WdfSpinLockAcquire(pContext->StatusQLock);
    h->Closing = TRUE;
    if (h->Enabled)
    {
        HapticsFaultLocked(pContext);
    }
    WdfSpinLockRelease(pContext->StatusQLock);
    if (h->Timer)
    {
        WdfTimerStop(h->Timer, TRUE);
    }
}

// Called only after reset, queue detachment and DPC/timer quiescence.
VOID VIOInputHapticsFree(PINPUT_DEVICE pContext)
{
    VIOINPUT_HAPTICS *h = &pContext->Haptics;
    ULONG i;
    for (i = 0; i < VIOINPUT_HAPTICS_SLOTS; ++i)
    {
        NT_ASSERT(!h->Slots[i].Busy && !h->Slots[i].Request);
    }
    if (h->Wire)
    {
        VirtIOWdfDeviceFreeDmaMemory(&pContext->VDevice.VIODevice, h->Wire);
        h->Wire = NULL;
    }
    DvhReceiverReset(&h->Receiver);
    DvhGuestReset(&h->State);
}
