/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 DroidVM contributors.
 * Exclusive-session bring-up: ordinary guest RAM, no rdmapool or user GPA input.
 */
#include "video.h"

/* Map both planar forms of a queue to its allocation table. */
static ULONG VvDirection(uint32_t queue)
{
    return (queue==VV_OUTPUT || queue==VV_OUTPUT_MPLANE)?1u:0u;
}

/* Drop stale notifications only after a corresponding CLOSE/STREAMOFF barrier. */
static void VvDiscardEvents(VV_DEVICE *d, uint32_t queue)
{
    ULONG n, kept=0, count;
    WdfSpinLockAcquire(d->lock);
    count=d->event_count;
    for (n=0;n<count;++n) {
        VV_EVENT e=d->events[(d->read_at+n)%VV_EVENT_RING];
        if (queue && !(e.event==VV_EVENT_DQBUF && e.queue==queue)) {
            d->events[(d->read_at+kept)%VV_EVENT_RING]=e; ++kept;
        }
    }
    d->event_count=kept;
    WdfSpinLockRelease(d->lock);
}

/* Free a direction only after the host has ended all access to these pages. */
static void VvFreeDirection(VV_DEVICE *d, ULONG direction)
{
    ULONG i;
    for (i=0;i<VV_MAX_BUFFERS;++i) {
        VV_ALLOCATION *b=&d->buffers[direction][i];
        if (b->va) {
            d->allocated-=b->ownership.capacity;
            MmFreeContiguousMemory(b->va);
            RtlZeroMemory(b,sizeof(*b));
        }
    }
    d->counts[direction]=0; d->queue_types[direction]=0;
}

/* Called only after CLOSE succeeds or the device reset has completed. */
void VvFreeBuffers(VV_DEVICE *d)
{
    VvFreeDirection(d,0); VvFreeDirection(d,1);
}

/* Forward a whitelisted fixed-size ioctl, checking response length and Linux errno. */
NTSTATUS VvIoctl(VV_DEVICE *d, uint32_t code, void *payload, ULONG input_size,
                 ULONG output_size, int32_t *error)
{
    UCHAR command[VV_COMMAND_BYTES]={0}, reply[VV_COMMAND_BYTES]={0};
    VV_IOCTL header={0};
    VV_RESPONSE response;
    ULONG used=0;
    NTSTATUS status;
    if (!d->have_session || input_size>208 || output_size>208 || !error) return STATUS_INVALID_DEVICE_STATE;
    header.header.command=VV_CMD_IOCTL; header.session=d->session; header.code=code;
    RtlCopyMemory(command,&header,sizeof(header));
    if (input_size) RtlCopyMemory(command+sizeof(header),payload,input_size);
    status=VvTransact(d,command,sizeof(header)+input_size,reply,sizeof(response)+output_size,&used);
    if (!NT_SUCCESS(status)) return status;
    RtlCopyMemory(&response,reply,sizeof(response));
    if (response.error<0 || response.error>4095 ||
        (!response.error && used!=sizeof(response)+output_size)) {
        InterlockedExchange(&d->faulted,1); return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    *error=response.error;
    if (!response.error && output_size) RtlCopyMemory(payload,reply+sizeof(response),output_size);
    return STATUS_SUCCESS;
}

/* A timed-out CLOSE must retain pages, because the host may still access them. */
NTSTATUS VvClose(VV_DEVICE *d)
{
    VV_IOCTL close={0}; VV_RESPONSE response={0};
    ULONG used=0; NTSTATUS status;
    if (!d->have_session) return STATUS_SUCCESS;
    close.header.command=VV_CMD_CLOSE; close.session=d->session;
    status=VvTransact(d,&close,sizeof(close),&response,sizeof(response),&used);
    /* Upstream returns the descriptor with used=0 only after close_session. */
    if (!NT_SUCCESS(status) || used!=0 || response.error) {
        InterlockedExchange(&d->faulted,1);
        return NT_SUCCESS(status)?STATUS_DEVICE_PROTOCOL_ERROR:status;
    }
    d->have_session=FALSE;
    VvDiscardEvents(d,0); VvFreeBuffers(d);
    return STATUS_SUCCESS;
}

/* Finish session teardown when the application's final file handle goes away. */
VOID VvCleanup(WDFFILEOBJECT file)
{
    VV_DEVICE *d=VvDevice(WdfFileObjectGetDevice(file));
    WdfWaitLockAcquire(d->gate,NULL);
    if (d->online) (void)VvClose(d);
    WdfWaitLockRelease(d->gate);
}

/* Allocate USERPTR backing only after honoring the count returned by REQBUFS. */
static NTSTATUS VvAllocate(VV_DEVICE *d, const VV_REQUEST *request, VV_RESULT *result)
{
    VV_FORMAT format={0}; VV_REQBUFS req={0};
    ULONG dir=VvDirection(request->queue), i;
    uint32_t size=0;
    PHYSICAL_ADDRESS low, high, boundary;
    NTSTATUS status;
    int32_t error=0;
    if (!vv_queue_valid(request->queue) || request->count>VV_MAX_BUFFERS) return STATUS_INVALID_PARAMETER;
    if (d->counts[dir]) return STATUS_INVALID_DEVICE_STATE; /* Close/reopen to release in v1. */
    if (!request->count) return STATUS_INVALID_PARAMETER;
    format.type=request->queue;
    status=VvIoctl(d,VV_IOCTL_G_FMT,&format,sizeof(format),sizeof(format),&result->error);
    if (!NT_SUCCESS(status) || result->error) return status;
    if (format.type!=request->queue || !vv_format_size(&format,&size)) return STATUS_NOT_SUPPORTED;
    req.count=request->count; req.type=request->queue; req.memory=VV_USERPTR;
    status=VvIoctl(d,VV_IOCTL_REQBUFS,&req,sizeof(req),sizeof(req),&result->error);
    if (!NT_SUCCESS(status) || result->error) return status;
    if (!req.count || req.count>VV_MAX_BUFFERS || req.type!=request->queue || req.memory!=VV_USERPTR ||
        (uint64_t)req.count*size>VV_MEMORY_LIMIT-d->allocated) {
        status=STATUS_INSUFFICIENT_RESOURCES; goto undo;
    }
    low.QuadPart=0; high.QuadPart=MAXLONGLONG; boundary.QuadPart=0;
    for (i=0;i<req.count;++i) {
        VV_ALLOCATION *b=&d->buffers[dir][i];
        b->va=MmAllocateContiguousMemorySpecifyCache(size,low,high,boundary,MmCached);
        if (!b->va) { status=STATUS_INSUFFICIENT_RESOURCES; goto undo; }
        b->pa=MmGetPhysicalAddress(b->va);
        b->ownership.capacity=size; b->ownership.state=VV_OWNED;
        d->allocated+=size;
        RtlZeroMemory(b->va,size);
    }
    d->counts[dir]=req.count; d->queue_types[dir]=request->queue;
    result->count=req.count; result->buffer_bytes=size;
    return STATUS_SUCCESS;
undo:
    /* No QBUF has been sent for these addresses, so allocation failure can free them. */
    VvFreeDirection(d,dir);
    req.count=0; req.type=request->queue; req.memory=VV_USERPTR;
    if (!NT_SUCCESS(VvIoctl(d,VV_IOCTL_REQBUFS,&req,sizeof(req),sizeof(req),&error)) || error)
        InterlockedExchange(&d->faulted,1);
    return status;
}

/* Return a buffer record only when queue, index and current session all match. */
static VV_ALLOCATION *VvBuffer(VV_DEVICE *d, uint32_t queue, uint32_t index)
{
    ULONG dir=VvDirection(queue);
    if (!d->have_session || !vv_queue_valid(queue) || d->queue_types[dir]!=queue || index>=d->counts[dir]) return NULL;
    return &d->buffers[dir][index];
}

/* QBUF response acknowledges submission, while DQBUF alone releases ownership. */
static NTSTATUS VvQueue(VV_DEVICE *d, const VV_REQUEST *request, VV_RESULT *result)
{
    VV_ALLOCATION *b=VvBuffer(d,request->queue,request->index);
    UCHAR command[VV_COMMAND_BYTES], reply[VV_COMMAND_BYTES];
    VV_RESPONSE response;
    size_t size=0, reply_size=0;
    ULONG used=0; NTSTATUS status;
    if (!b || b->ownership.state==VV_HOST ||
        !vv_build_qbuf(command,sizeof(command),d->session,request->queue,request->index,
                       (uint64_t)b->pa.QuadPart,b->ownership.capacity,request->bytesused,
                       request->timestamp_us,&size,&reply_size)) return STATUS_INVALID_PARAMETER;
    if (!vv_buffer_submit(&b->ownership)) return STATUS_INVALID_DEVICE_STATE;
    KeMemoryBarrier();
    status=VvTransact(d,command,(ULONG)size,reply,(ULONG)reply_size,&used);
    if (!NT_SUCCESS(status)) {
        /* Even queue-add failures fail closed: don't guess whether DMA happened. */
        InterlockedExchange(&d->faulted,1); return status;
    }
    RtlCopyMemory(&response,reply,sizeof(response));
    if (response.error<0 || response.error>4095 || (!response.error && used!=reply_size)) {
        InterlockedExchange(&d->faulted,1); return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    result->error=response.error;
    if (response.error) b->ownership.state=VV_OWNED; /* Explicit host rejection, not timeout. */
    return STATUS_SUCCESS;
}

/* Restrict generic controls to pointer-free codec payloads and legal queue types. */
static NTSTATUS VvGeneric(VV_DEVICE *d, const VV_REQUEST *request, VV_RESULT *result)
{
    uint32_t in=0,out=0,type=0;
    if (!vv_ioctl_shape(request->code,&in,&out) || in!=request->payload_size) return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(result->payload,request->payload,in);
    switch (request->code) {
    case VV_IOCTL_ENUM_FMT:
        RtlCopyMemory(&type,request->payload+4,sizeof(type)); break;
    case VV_IOCTL_G_FMT: case VV_IOCTL_S_FMT: case VV_IOCTL_TRY_FMT:
    case VV_IOCTL_G_PARM: case VV_IOCTL_S_PARM: case VV_IOCTL_G_SELECTION:
        RtlCopyMemory(&type,request->payload,sizeof(type)); break;
    default: break;
    }
    if (type && !vv_queue_valid(type)) return STATUS_INVALID_PARAMETER;
    if ((request->code==VV_IOCTL_G_FMT || request->code==VV_IOCTL_S_FMT || request->code==VV_IOCTL_TRY_FMT ||
         request->code==VV_IOCTL_G_PARM || request->code==VV_IOCTL_S_PARM || request->code==VV_IOCTL_G_SELECTION ||
         request->code==VV_IOCTL_ENUM_FMT) && !type) return STATUS_INVALID_PARAMETER;
    if (request->code==VV_IOCTL_S_FMT && d->counts[VvDirection(type)]) return STATUS_DEVICE_BUSY;
    result->payload_size=out;
    return VvIoctl(d,request->code,result->payload,in,out,&result->error);
}

/* Execute one versioned request under the per-device passive-level gate. */
static NTSTATUS VvExecute(VV_DEVICE *d, const VV_REQUEST *request, VV_RESULT *result)
{
    NTSTATUS status;
    ULONG used=0,i,dir;
    uint32_t queue;
    if (request->version!=VV_API_VERSION) return STATUS_REVISION_MISMATCH;
    result->version=VV_API_VERSION;
    if (!d->online || d->faulted) return STATUS_DEVICE_NOT_READY;
    if (request->operation==VV_OP_CONFIG) { result->config=d->config; return STATUS_SUCCESS; }
    if (request->operation==VV_OP_OPEN) {
        VV_COMMAND open={VV_CMD_OPEN,0}; VV_OPEN_RESPONSE response={0};
        if (d->have_session) return STATUS_DEVICE_BUSY;
        status=VvTransact(d,&open,sizeof(open),&response,sizeof(response),&used);
        if (!NT_SUCCESS(status)) return status;
        if (response.header.error<0 || response.header.error>4095 ||
            (!response.header.error && used!=sizeof(response))) {
            InterlockedExchange(&d->faulted,1); return STATUS_DEVICE_PROTOCOL_ERROR;
        }
        result->error=response.header.error;
        if (!result->error) { d->session=response.session; d->have_session=TRUE; }
        return STATUS_SUCCESS;
    }
    if (!d->have_session) return STATUS_INVALID_DEVICE_STATE;
    switch (request->operation) {
    case VV_OP_CLOSE: return VvClose(d);
    case VV_OP_IOCTL: return VvGeneric(d,request,result);
    case VV_OP_ALLOC: return VvAllocate(d,request,result);
    case VV_OP_QBUF: return VvQueue(d,request,result);
    case VV_OP_STREAMON: case VV_OP_STREAMOFF:
        queue=request->queue;
        if (!vv_queue_valid(queue)) return STATUS_INVALID_PARAMETER;
        dir=VvDirection(queue);
        if (d->queue_types[dir]!=queue) return STATUS_INVALID_DEVICE_STATE;
        status=VvIoctl(d,request->operation==VV_OP_STREAMON?VV_IOCTL_STREAMON:VV_IOCTL_STREAMOFF,
                       &queue,sizeof(queue),0,&result->error);
        if (NT_SUCCESS(status) && !result->error && request->operation==VV_OP_STREAMOFF) {
            VvDiscardEvents(d,queue);
            for (i=0;i<d->counts[dir];++i) d->buffers[dir][i].ownership.state=VV_OWNED;
        }
        return status;
    case VV_OP_EVENT:
        WdfSpinLockAcquire(d->lock);
        if (!d->event_count) { WdfSpinLockRelease(d->lock); result->error=11; return STATUS_SUCCESS; }
        result->event=d->events[d->read_at];
        d->read_at=(d->read_at+1)%VV_EVENT_RING; --d->event_count;
        WdfSpinLockRelease(d->lock);
        if (result->event.session!=d->session) goto protocol_error;
        if (result->event.event==VV_EVENT_DQBUF) {
            VV_ALLOCATION *b=VvBuffer(d,result->event.queue,result->event.index);
            if (!b || !vv_buffer_return(&b->ownership,&result->event)) goto protocol_error;
            KeMemoryBarrier();
        } else if (result->event.event==VV_EVENT_ERROR) {
            InterlockedExchange(&d->faulted,1);
        }
        return STATUS_SUCCESS;
    default: return STATUS_INVALID_DEVICE_REQUEST;
    }
protocol_error:
    InterlockedExchange(&d->faulted,1);
    return STATUS_DEVICE_PROTOCOL_ERROR;
}

/* Copy only while Windows owns the buffer; no arbitrary user addresses are accepted. */
static NTSTATUS VvCopy(VV_DEVICE *d, WDFREQUEST request, ULONG code, ULONG_PTR *information)
{
    PVOID input,output;
    size_t input_size=0,output_size=0;
    VV_COPY copy;
    VV_ALLOCATION *b;
    NTSTATUS status;
    if (!d->online || d->faulted) return STATUS_DEVICE_NOT_READY;
    status=WdfRequestRetrieveInputBuffer(request,sizeof(copy),&input,&input_size);
    if (!NT_SUCCESS(status)) return status;
    RtlCopyMemory(&copy,input,sizeof(copy));
    b=VvBuffer(d,copy.queue,copy.index);
    if (!b || !copy.length || copy.length>VV_COPY_LIMIT || b->ownership.state==VV_HOST ||
        copy.offset>b->ownership.capacity || copy.length>b->ownership.capacity-copy.offset) return STATUS_INVALID_PARAMETER;
    if (code==IOCTL_VV_WRITE) {
        if (VvDirection(copy.queue)!=1 || input_size-sizeof(copy)!=copy.length) return STATUS_INVALID_PARAMETER;
        RtlCopyMemory((PUCHAR)b->va+copy.offset,(PUCHAR)input+sizeof(copy),copy.length);
    } else {
        if (VvDirection(copy.queue)!=0 || b->ownership.state!=VV_RETURNED || input_size!=sizeof(copy)) return STATUS_INVALID_PARAMETER;
        status=WdfRequestRetrieveOutputBuffer(request,copy.length,&output,&output_size);
        if (!NT_SUCCESS(status)) return status;
        RtlCopyMemory(output,(PUCHAR)b->va+copy.offset,copy.length);
        *information=copy.length;
    }
    return STATUS_SUCCESS;
}

/* Dispatch the private ABI; all potentially blocking work stays at PASSIVE_LEVEL. */
VOID VvControl(WDFQUEUE queue, WDFREQUEST request, size_t out_length, size_t in_length, ULONG code)
{
    VV_DEVICE *d=VvDevice(WdfIoQueueGetDevice(queue));
    NTSTATUS status=STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information=0;
    UNREFERENCED_PARAMETER(out_length); UNREFERENCED_PARAMETER(in_length);
    WdfWaitLockAcquire(d->gate,NULL);
    if (code==IOCTL_VV_EXEC) {
        VV_REQUEST local;
        VV_REQUEST *input;
        VV_RESULT *output;
        size_t length;
        status=WdfRequestRetrieveInputBuffer(request,sizeof(local),(PVOID *)&input,&length);
        if (NT_SUCCESS(status) && length!=sizeof(local)) status=STATUS_INVALID_PARAMETER;
        if (NT_SUCCESS(status)) {
            RtlCopyMemory(&local,input,sizeof(local));
            status=WdfRequestRetrieveOutputBuffer(request,sizeof(*output),(PVOID *)&output,NULL);
            if (NT_SUCCESS(status)) {
                RtlZeroMemory(output,sizeof(*output));
                status=VvExecute(d,&local,output);
                if (NT_SUCCESS(status)) information=sizeof(*output);
            }
        }
    } else if (code==IOCTL_VV_WRITE || code==IOCTL_VV_READ) status=VvCopy(d,request,code,&information);
    WdfWaitLockRelease(d->gate);
    WdfRequestCompleteWithInformation(request,status,information);
}
