/* SPDX-License-Identifier: BSD-3-Clause
 * VioGPU video transport for DroidVM unprotected ARM64 guests.
 * Media is a separate VirtIO function (ID 48), not a GPU controlq extension.
 * One exclusive session per media function. No arbitrary USERPTRs from user mode.
 */
#define INITGUID
#include <ntddk.h>
#include <wdf.h>
#include <wdmguid.h>
#include <ntstrsafe.h>
#include "osdep.h"
#include "virtio_pci.h"
#include "VirtIO.h"
#include "VirtIOWdf.h"
#include "../video/media_state.h"

#define VIDEO_TAG ((ULONG)'vdGV')
#define VIDEO_DMA_BYTES 4096u
#define VIDEO_EVENT_SLOTS 16u
#define VIDEO_EVENT_RING 128u
#define VIDEO_ALIGN 65536u
#define VIDEO_COMMAND_TIMEOUT_MS 15000u
static const GUID VideoInterface=VGPU_VIDEO_INTERFACE_GUID_INIT;

typedef struct VIDEO_BUFFER {
    PVOID Allocation, Address;
    PHYSICAL_ADDRESS Gpa;
    SIZE_T AllocationBytes;
    VGPU_BUFFER_TRACKER Tracker;
} VIDEO_BUFFER;
typedef struct VIDEO_EVENT_SLOT { PVOID Address; PHYSICAL_ADDRESS Dma; } VIDEO_EVENT_SLOT;
typedef struct VIDEO_EVENT_COPY { ULONG Length; UCHAR Bytes[sizeof(VMEDIA_EVENT)]; } VIDEO_EVENT_COPY;
typedef struct VIDEO_DEVICE {
    VIRTIO_WDF_DRIVER Virtio;
    WDFINTERRUPT Interrupt;
    WDFSPINLOCK QueueLock;
    WDFWAITLOCK SessionLock;
    struct virtqueue *Queues[2];
    KEVENT CommandDone, EventReady;
    PVOID Command, Response;
    PHYSICAL_ADDRESS CommandDma, ResponseDma;
    VIDEO_EVENT_SLOT EventSlots[VIDEO_EVENT_SLOTS];
    VIDEO_EVENT_COPY Events[VIDEO_EVENT_RING];
    ULONG EventHead, EventTail, EventCount, UsedBytes;
    LONG Ready, Poisoned, EventFailed;
    BOOLEAN Initialized, CommandPending, Opened, Streaming[2];
    ULONG SessionId, Count[2];
    ULONGLONG Generation, AllocatedBytes;
    VIDEO_BUFFER Buffers[2][VGPU_VIDEO_MAX_BUFFERS];
} VIDEO_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VIDEO_DEVICE, VideoGetDevice);
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD VideoAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE VideoPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE VideoRelease;
EVT_WDF_DEVICE_D0_ENTRY VideoD0Entry;
EVT_WDF_DEVICE_D0_EXIT VideoD0Exit;
EVT_WDF_INTERRUPT_ISR VideoIsr;
EVT_WDF_INTERRUPT_DPC VideoDpc;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL VideoIoctl;
EVT_WDF_FILE_CLEANUP VideoCleanup;

/* VirtioLib's diagnostic hook; never dump media contents or addresses. */
static void VideoDebug(const char *format, ...)
{
    va_list ap;
    va_start(ap,format);
    vDbgPrintEx(DPFLTR_IHVDRIVER_ID,DPFLTR_WARNING_LEVEL,format,ap);
    va_end(ap);
}
void (*VirtioDebugPrintProc)(const char *, ...)=VideoDebug;
int virtioDebugLevel=1;
int bDebugPrint=1;

/* Initialize a returned private ABI header. */
static void VideoHeader(VGPU_VIDEO_HEADER *h, ULONG size, ULONGLONG generation)
{
    h->Version=VGPU_VIDEO_VERSION; h->Size=size; h->Generation=generation;
}
/* Convert a millisecond timeout to a relative kernel timeout. */
static LARGE_INTEGER VideoDeadline(ULONG milliseconds)
{
    LARGE_INTEGER value; value.QuadPart=-(LONGLONG)milliseconds*10000; return value;
}
/* Enqueue an owned event descriptor. Called only under QueueLock. */
static int VideoPostEvent(VIDEO_DEVICE *d, VIDEO_EVENT_SLOT *slot)
{
    struct scatterlist sg;
    sg.physAddr=slot->Dma; sg.length=(ULONG)sizeof(VMEDIA_EVENT);
    return virtqueue_add_buf(d->Queues[1],&sg,0,1,slot,NULL,0);
}
/* Retain event ownership locally and immediately replenish eventq. */
static void VideoConsumeEvents(VIDEO_DEVICE *d)
{
    VIDEO_EVENT_SLOT *slot;
    unsigned int length;
    virtqueue_disable_cb(d->Queues[1]);
    do {
        while ((slot=(VIDEO_EVENT_SLOT *)virtqueue_get_buf(d->Queues[1],&length))!=NULL) {
            if (length>=8 && length<=sizeof(VMEDIA_EVENT) && d->EventCount<VIDEO_EVENT_RING) {
                VIDEO_EVENT_COPY *copy=&d->Events[d->EventTail];
                copy->Length=length;
                RtlCopyMemory(copy->Bytes,slot->Address,length);
                d->EventTail=(d->EventTail+1)%VIDEO_EVENT_RING; ++d->EventCount;
            } else {
                InterlockedExchange(&d->EventFailed,1);
            }
            RtlZeroMemory(slot->Address,sizeof(VMEDIA_EVENT));
            if (VideoPostEvent(d,slot)<0) InterlockedExchange(&d->EventFailed,1);
            KeSetEvent(&d->EventReady,IO_NO_INCREMENT,FALSE);
        }
    } while (!virtqueue_enable_cb(d->Queues[1]));
    virtqueue_kick(d->Queues[1]);
}
/* Drain all events before publishing command completion, including CLOSE's used entry. */
void VideoDpc(WDFINTERRUPT interrupt, WDFOBJECT associated)
{
    VIDEO_DEVICE *d=VideoGetDevice(WdfInterruptGetDevice(interrupt));
    void *token; unsigned int length;
    UNREFERENCED_PARAMETER(associated);
    WdfSpinLockAcquire(d->QueueLock);
    if (d->Ready) {
        VideoConsumeEvents(d);
        virtqueue_disable_cb(d->Queues[0]);
        do {
            while ((token=virtqueue_get_buf(d->Queues[0],&length))!=NULL) {
                if (token==&d->CommandPending && d->CommandPending) {
                    d->UsedBytes=length; d->CommandPending=FALSE;
                    KeSetEvent(&d->CommandDone,IO_NO_INCREMENT,FALSE);
                } else InterlockedExchange(&d->Poisoned,1);
            }
        } while (!virtqueue_enable_cb(d->Queues[0]));
    }
    WdfSpinLockRelease(d->QueueLock);
}
/* Acknowledge line interrupts; message-signalled interrupts always schedule a drain. */
BOOLEAN VideoIsr(WDFINTERRUPT interrupt, ULONG messageId)
{
    VIDEO_DEVICE *d=VideoGetDevice(WdfInterruptGetDevice(interrupt));
    WDF_INTERRUPT_INFO info;
    UNREFERENCED_PARAMETER(messageId);
    if (!InterlockedCompareExchange(&d->Ready,0,0)) return FALSE;
    WDF_INTERRUPT_INFO_INIT(&info); WdfInterruptGetInfo(interrupt,&info);
    if (info.MessageSignaled || VirtIOWdfGetISRStatus(&d->Virtio)) {
        WdfInterruptQueueDpcForIsr(interrupt); return TRUE;
    }
    return FALSE;
}
/* Serialize commandq. On timeout its DMA buffers remain pinned until a device reset. */
static NTSTATUS VideoCommand(
    VIDEO_DEVICE *d, const void *request, ULONG requestBytes,
    void *response, ULONG responseBytes, ULONG *written)
{
    struct scatterlist sg[2];
    LARGE_INTEGER timeout=VideoDeadline(VIDEO_COMMAND_TIMEOUT_MS);
    NTSTATUS status; int result;
    *written=0;
    if (!d->Ready || d->Poisoned) return STATUS_DEVICE_NOT_READY;
    if (requestBytes>VIDEO_DMA_BYTES || responseBytes>VIDEO_DMA_BYTES) return STATUS_INVALID_BUFFER_SIZE;
    RtlCopyMemory(d->Command,request,requestBytes);
    RtlZeroMemory(d->Response,VIDEO_DMA_BYTES);
    sg[0].physAddr=d->CommandDma; sg[0].length=requestBytes;
    sg[1].physAddr=d->ResponseDma; sg[1].length=responseBytes;
    WdfSpinLockAcquire(d->QueueLock);
    KeClearEvent(&d->CommandDone); d->UsedBytes=0; d->CommandPending=TRUE;
    result=virtqueue_add_buf(d->Queues[0],sg,1,responseBytes?1:0,&d->CommandPending,NULL,0);
    if (result>=0) virtqueue_kick(d->Queues[0]); else d->CommandPending=FALSE;
    WdfSpinLockRelease(d->QueueLock);
    if (result<0) return STATUS_DEVICE_BUSY;
    status=KeWaitForSingleObject(&d->CommandDone,Executive,KernelMode,FALSE,&timeout);
    if (status!=STATUS_SUCCESS) {
        InterlockedExchange(&d->Poisoned,1);
        DbgPrintEx(DPFLTR_IHVDRIVER_ID,DPFLTR_ERROR_LEVEL,
                   "VioGPU video: command timeout; buffers retained until device reset\n");
        return STATUS_IO_TIMEOUT;
    }
    KeMemoryBarrier();
    if (!d->Ready || d->Poisoned || d->UsedBytes>responseBytes) {
        InterlockedExchange(&d->Poisoned,1); return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    *written=d->UsedBytes;
    if (responseBytes && *written) RtlCopyMemory(response,d->Response,*written);
    return STATUS_SUCCESS;
}
/* Run one fixed-format ioctl; Linux errno stays distinct from NT transport status. */
static NTSTATUS VideoControl(
    VIDEO_DEVICE *d, ULONG code, void *payload, ULONG bytes, BOOLEAN hasReply, uint32_t *error)
{
    UCHAR request[sizeof(VMEDIA_IOCTL)+VGPU_VIDEO_CONTROL_BYTES];
    UCHAR response[sizeof(VMEDIA_RESP)+VGPU_VIDEO_CONTROL_BYTES];
    VMEDIA_IOCTL command; ULONG written; NTSTATUS status;
    RtlZeroMemory(&command,sizeof(command)); command.Header.Command=VMEDIA_CMD_IOCTL;
    command.SessionId=d->SessionId; command.Code=code;
    if (bytes>VGPU_VIDEO_CONTROL_BYTES) return STATUS_INVALID_BUFFER_SIZE;
    RtlCopyMemory(request,&command,sizeof(command));
    RtlCopyMemory(request+sizeof(command),payload,bytes);
    *error=0;
    status=VideoCommand(d,request,(ULONG)sizeof(command)+bytes,response,
        (ULONG)sizeof(VMEDIA_RESP)+(hasReply?bytes:0),&written);
    if (!NT_SUCCESS(status)) return status;
    if (written<sizeof(VMEDIA_RESP)) goto malformed;
    *error=VmediaRead32(response);
    if (hasReply && written==sizeof(VMEDIA_RESP)+bytes)
        RtlCopyMemory(payload,response+sizeof(VMEDIA_RESP),bytes);
    else if (hasReply && !*error) goto malformed;
    return STATUS_SUCCESS;
malformed:
    InterlockedExchange(&d->Poisoned,1); return STATUS_DEVICE_PROTOCOL_ERROR;
}
/* Release only buffers whose host references were revoked by REQBUFS(0), CLOSE or reset. */
static void VideoFreeBuffers(VIDEO_DEVICE *d, int q)
{
    ULONG i;
    for (i=0;i<VGPU_VIDEO_MAX_BUFFERS;++i) {
        VIDEO_BUFFER *b=&d->Buffers[q][i];
        if (b->Allocation) {
            RtlSecureZeroMemory(b->Address,b->Tracker.Capacity);
            MmFreeContiguousMemory(b->Allocation);
            d->AllocatedBytes-=b->AllocationBytes;
        }
        RtlZeroMemory(b,sizeof(*b));
    }
    d->Count[q]=0;
}
/* Purge completed events from an obsolete capture/output queue without losing the other queue. */
static void VideoPurgeEvents(VIDEO_DEVICE *d, int q)
{
    ULONG count,i;
    WdfSpinLockAcquire(d->QueueLock);
    if (d->Ready) VideoConsumeEvents(d);
    count=d->EventCount;
    for (i=0;i<count;++i) {
        VIDEO_EVENT_COPY copy=d->Events[d->EventHead];
        d->EventHead=(d->EventHead+1)%VIDEO_EVENT_RING; --d->EventCount;
        if (q<0 || (copy.Length>=sizeof(VMEDIA_BUFFER)+8 && VmediaRead32(copy.Bytes)==VMEDIA_EVT_DQBUF &&
            VmediaQueueIndex(VmediaRead32(copy.Bytes+8+4))==q)) continue;
        d->Events[d->EventTail]=copy; d->EventTail=(d->EventTail+1)%VIDEO_EVENT_RING; ++d->EventCount;
    }
    if (!d->EventCount) KeClearEvent(&d->EventReady);
    WdfSpinLockRelease(d->QueueLock);
}
/* CLOSE has no response body in virtio-media; completion is its returned descriptor. */
static NTSTATUS VideoClose(VIDEO_DEVICE *d)
{
    VMEDIA_IOCTL close; ULONG written; NTSTATUS status=STATUS_SUCCESS;
    if (d->Opened) {
        RtlZeroMemory(&close,sizeof(close)); close.Header.Command=VMEDIA_CMD_CLOSE;
        close.SessionId=d->SessionId;
        status=VideoCommand(d,&close,sizeof(close),NULL,0,&written);
    }
    d->Opened=FALSE; d->Streaming[0]=FALSE; d->Streaming[1]=FALSE;
    ++d->Generation;
    if (NT_SUCCESS(status) && !d->Poisoned) {
        VideoFreeBuffers(d,0); VideoFreeBuffers(d,1); VideoPurgeEvents(d,-1);
        InterlockedExchange(&d->EventFailed,0);
    }
    return status;
}
/* Allocate aligned ordinary guest RAM; the SG carries GPA, never a WDF DMA logical address. */
static NTSTATUS VideoAllocate(VIDEO_DEVICE *d, VGPU_VIDEO_BUFFERS *a)
{
    VMEDIA_FORMAT format; VMEDIA_REQBUFS req;
    uint32_t error=0,size=0; ULONG i; int q=VmediaQueueIndex(a->Type); NTSTATUS status;
    PHYSICAL_ADDRESS low,high,boundary;
    if (q<0 || a->Count>VGPU_VIDEO_MAX_BUFFERS || d->Streaming[q]) return STATUS_INVALID_PARAMETER;
    if (a->Count && d->Count[q]) return STATUS_DEVICE_BUSY;
    RtlZeroMemory(&format,sizeof(format)); format.Type=a->Type;
    if (a->Count) {
        status=VideoControl(d,4,&format,sizeof(format),TRUE,&error);
        if (!NT_SUCCESS(status)) return status;
        if (error) { a->LinuxErrno=error; return STATUS_SUCCESS; }
        if (!VmediaFormatSize(&format,VGPU_VIDEO_MAX_BUFFER_BYTES,&size)) return STATUS_NOT_SUPPORTED;
    }
    RtlZeroMemory(&req,sizeof(req)); req.Count=a->Count; req.Type=a->Type; req.Memory=VMEDIA_MEMORY_USERPTR;
    status=VideoControl(d,8,&req,sizeof(req),TRUE,&error); a->LinuxErrno=error;
    if (!NT_SUCCESS(status) || error) return status;
    if (!a->Count) {
        VideoFreeBuffers(d,q); VideoPurgeEvents(d,q); a->BufferBytes=0; return STATUS_SUCCESS;
    }
    if (!req.Count || req.Count>VGPU_VIDEO_MAX_BUFFERS || req.Type!=a->Type || req.Memory!=VMEDIA_MEMORY_USERPTR) {
        status=STATUS_DEVICE_PROTOCOL_ERROR; goto rollback;
    }
    low.QuadPart=0; high.QuadPart=MAXLONGLONG; boundary.QuadPart=0;
    for (i=0;i<req.Count;++i) {
        VIDEO_BUFFER *b=&d->Buffers[q][i];
        SIZE_T rounded=((SIZE_T)size+VIDEO_ALIGN-1)&~((SIZE_T)VIDEO_ALIGN-1);
        SIZE_T total=rounded+VIDEO_ALIGN;
        ULONGLONG pa,aligned;
        if (d->AllocatedBytes+total>VGPU_VIDEO_MAX_TOTAL_BYTES) { status=STATUS_INSUFFICIENT_RESOURCES; goto rollback; }
        b->Allocation=MmAllocateContiguousMemorySpecifyCache(total,low,high,boundary,MmCached);
        if (!b->Allocation) { status=STATUS_INSUFFICIENT_RESOURCES; goto rollback; }
        b->AllocationBytes=total; d->AllocatedBytes+=total;
        pa=(ULONGLONG)MmGetPhysicalAddress(b->Allocation).QuadPart;
        aligned=(pa+VIDEO_ALIGN-1)&~((ULONGLONG)VIDEO_ALIGN-1);
        b->Address=(UCHAR *)b->Allocation+(SIZE_T)(aligned-pa); b->Gpa.QuadPart=(LONGLONG)aligned;
        b->Tracker.Capacity=size; b->Tracker.State=VgpuBufferIdle;
        RtlZeroMemory(b->Address,rounded);
    }
    d->Count[q]=req.Count; a->Count=req.Count; a->BufferBytes=size;
    return STATUS_SUCCESS;
rollback:
    /* No QBUF has exposed any allocation yet, so local rollback is always safe. */
    VideoFreeBuffers(d,q);
    req.Count=0; (void)VideoControl(d,8,&req,sizeof(req),TRUE,&error);
    return status;
}
/* Copy an input into retained storage, then lend its GPA; never retain a WDF request buffer. */
static NTSTATUS VideoQueueBuffer(VIDEO_DEVICE *d, const VGPU_VIDEO_BUFFER *r, SIZE_T inputBytes)
{
    UCHAR packet[184],response[sizeof(VMEDIA_RESP)+sizeof(VMEDIA_BUFFER)+sizeof(VMEDIA_PLANE)];
    int q=VmediaQueueIndex(r->Type),packetBytes;
    ULONG written,error; NTSTATUS status; VIDEO_BUFFER *b;
    if (q<0 || r->Index>=d->Count[q] || r->Flags) return STATUS_INVALID_PARAMETER;
    b=&d->Buffers[q][r->Index];
    if (r->BytesUsed>b->Tracker.Capacity || (q==1 && r->BytesUsed) ||
        inputBytes!=sizeof(*r)+(q==0?r->BytesUsed:0)) return STATUS_INVALID_BUFFER_SIZE;
    if (!VgpuVideoBeginQueue(&b->Tracker)) return STATUS_DEVICE_BUSY;
    if (q==0) RtlCopyMemory(b->Address,r+1,r->BytesUsed);
    packetBytes=VmediaBuildQbuf(d->SessionId,r->Type,r->Index,(uint64_t)b->Gpa.QuadPart,
        b->Tracker.Capacity,r->BytesUsed,r->TimestampUs,packet,sizeof(packet));
    if (!packetBytes) { b->Tracker.State=VgpuBufferIdle; return STATUS_INVALID_PARAMETER; }
    KeMemoryBarrier();
    status=VideoCommand(d,packet,(ULONG)packetBytes,response,sizeof(response),&written);
    if (!NT_SUCCESS(status)) {
        /* A full command queue never published a descriptor. Timeouts did. */
        if (status==STATUS_DEVICE_BUSY && !d->Poisoned) b->Tracker.State=VgpuBufferIdle;
        return status;
    }
    if (written<sizeof(VMEDIA_RESP)) { InterlockedExchange(&d->Poisoned,1); return STATUS_DEVICE_PROTOCOL_ERROR; }
    error=VmediaRead32(response);
    if (error) { b->Tracker.State=VgpuBufferIdle; return error==11?STATUS_DEVICE_BUSY:STATUS_UNSUCCESSFUL; }
    if (written!=sizeof(response)) { InterlockedExchange(&d->Poisoned,1); return STATUS_DEVICE_PROTOCOL_ERROR; }
    return STATUS_SUCCESS;
}
/* Consume one sanitized completion, never exposing host pointers or untrusted buffer offsets. */
static NTSTATUS VideoGetEvent(VIDEO_DEVICE *d, VGPU_VIDEO_EVENT *e)
{
    VIDEO_EVENT_COPY copy; VGPU_VIDEO_EVENT result;
    LARGE_INTEGER timeout; NTSTATUS status; uint32_t offset; int q;
    if (e->TimeoutMs>1000) return STATUS_INVALID_PARAMETER;
    WdfSpinLockAcquire(d->QueueLock);
    if (!d->EventCount) KeClearEvent(&d->EventReady);
    WdfSpinLockRelease(d->QueueLock);
    timeout=VideoDeadline(e->TimeoutMs);
    status=KeWaitForSingleObject(&d->EventReady,Executive,KernelMode,FALSE,&timeout);
    if (status!=STATUS_SUCCESS) return STATUS_NO_MORE_ENTRIES;
    WdfSpinLockAcquire(d->QueueLock);
    if (d->EventFailed) { WdfSpinLockRelease(d->QueueLock); return STATUS_DEVICE_PROTOCOL_ERROR; }
    if (!d->EventCount) { WdfSpinLockRelease(d->QueueLock); return STATUS_NO_MORE_ENTRIES; }
    copy=d->Events[d->EventHead]; d->EventHead=(d->EventHead+1)%VIDEO_EVENT_RING; --d->EventCount;
    if (!d->EventCount) KeClearEvent(&d->EventReady);
    WdfSpinLockRelease(d->QueueLock);
    if (!VgpuVideoParseEvent(copy.Bytes,copy.Length,d->SessionId,&result,&offset)) goto malformed;
    if (result.Event==VMEDIA_EVT_DQBUF) {
        q=VmediaQueueIndex(result.Type);
        if (q<0 || result.Index>=d->Count[q] ||
            !VgpuVideoCompleteBuffer(&d->Buffers[q][result.Index].Tracker,&result,offset)) goto malformed;
    }
    VideoHeader(&result.Header,sizeof(result),d->Generation); *e=result;
    if (result.Event==VMEDIA_EVT_ERROR) InterlockedExchange(&d->EventFailed,1);
    return STATUS_SUCCESS;
malformed:
    InterlockedExchange(&d->EventFailed,1); return STATUS_DEVICE_PROTOCOL_ERROR;
}
/* Validate the type field of control payloads which contain a format/queue union. */
static BOOLEAN VideoControlTypeValid(const VGPU_VIDEO_CONTROL *c)
{
    switch (c->Code) {
    case 2: return VmediaQueueIndex(VmediaRead32(c->Payload+4))>=0;
    case 4: case 5: case 64: case 21: case 22: case 94: case 95:
        return VmediaQueueIndex(VmediaRead32(c->Payload))>=0;
    default: return TRUE;
    }
}
/* Dispatch private IOCTLs at PASSIVE_LEVEL; all lengths and generations are checked first. */
void VideoIoctl(WDFQUEUE queue, WDFREQUEST request, size_t outputBytes, size_t inputBytes, ULONG code)
{
    VIDEO_DEVICE *d=VideoGetDevice(WdfIoQueueGetDevice(queue));
    VGPU_VIDEO_HEADER *in=NULL; PVOID out=NULL; SIZE_T received=0; ULONG_PTR information=0;
    NTSTATUS status; ULONG expected=0;
    status=WdfRequestRetrieveInputBuffer(request,sizeof(*in),(PVOID *)&in,&received);
    if (!NT_SUCCESS(status)) goto done;
    switch(code) {
    case IOCTL_VGPU_VIDEO_OPEN: expected=sizeof(VGPU_VIDEO_HEADER); break;
    case IOCTL_VGPU_VIDEO_CONTROL: expected=sizeof(VGPU_VIDEO_CONTROL); break;
    case IOCTL_VGPU_VIDEO_BUFFERS: expected=sizeof(VGPU_VIDEO_BUFFERS); break;
    case IOCTL_VGPU_VIDEO_QUEUE: case IOCTL_VGPU_VIDEO_COPY: expected=sizeof(VGPU_VIDEO_BUFFER); break;
    case IOCTL_VGPU_VIDEO_EVENT: expected=sizeof(VGPU_VIDEO_EVENT); break;
    case IOCTL_VGPU_VIDEO_STREAM: expected=sizeof(VGPU_VIDEO_STREAM); break;
    case IOCTL_VGPU_VIDEO_CLOSE: expected=sizeof(VGPU_VIDEO_HEADER); break;
    default: status=STATUS_INVALID_DEVICE_REQUEST; goto done;
    }
    WdfWaitLockAcquire(d->SessionLock,NULL);
    if (!VgpuVideoHeaderValid(in,inputBytes,expected,code==IOCTL_VGPU_VIDEO_OPEN?0:d->Generation)) {
        status=STATUS_INVALID_PARAMETER; goto unlock;
    }
    if (code!=IOCTL_VGPU_VIDEO_QUEUE && inputBytes!=expected) { status=STATUS_INVALID_BUFFER_SIZE; goto unlock; }
    if (!d->Ready || d->Poisoned || (code!=IOCTL_VGPU_VIDEO_OPEN && !d->Opened)) {
        status=STATUS_DEVICE_NOT_READY; goto unlock;
    }
    if (d->EventFailed && code!=IOCTL_VGPU_VIDEO_CLOSE) { status=STATUS_DEVICE_PROTOCOL_ERROR; goto unlock; }
    if (code==IOCTL_VGPU_VIDEO_OPEN) {
        VMEDIA_CMD command={VMEDIA_CMD_OPEN,0}; VMEDIA_OPEN_RESP response;
        VGPU_VIDEO_OPEN result; ULONG written;
        if (d->Opened) { status=STATUS_DEVICE_BUSY; goto unlock; }
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(result),&out,NULL);
        if (!NT_SUCCESS(status)) goto unlock;
        VideoPurgeEvents(d,-1);
        status=VideoCommand(d,&command,sizeof(command),&response,sizeof(response),&written);
        if (!NT_SUCCESS(status)) goto unlock;
        if (written<sizeof(VMEDIA_RESP) || (response.Header.Errno==0 && written!=sizeof(response))) {
            InterlockedExchange(&d->Poisoned,1); status=STATUS_DEVICE_PROTOCOL_ERROR; goto unlock;
        }
        if (response.Header.Errno) { status=STATUS_DEVICE_BUSY; goto unlock; }
        d->SessionId=response.SessionId; d->Opened=TRUE; ++d->Generation;
        RtlZeroMemory(&result,sizeof(result)); VideoHeader(&result.Header,sizeof(result),d->Generation);
        VirtIOWdfDeviceGet(&d->Virtio,0,&result.Config,sizeof(result.Config));
        RtlCopyMemory(out,&result,sizeof(result)); information=sizeof(result);
    } else if (code==IOCTL_VGPU_VIDEO_CLOSE) {
        status=VideoClose(d);
    } else if (code==IOCTL_VGPU_VIDEO_CONTROL) {
        VGPU_VIDEO_CONTROL c=*(VGPU_VIDEO_CONTROL *)in;
        if (c.Reserved || !VmediaControlSize(c.Code) || c.PayloadBytes!=VmediaControlSize(c.Code) || !VideoControlTypeValid(&c)) {
            status=STATUS_INVALID_PARAMETER; goto unlock;
        }
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(c),&out,NULL);
        if (!NT_SUCCESS(status)) goto unlock;
        status=VideoControl(d,c.Code,c.Payload,c.PayloadBytes,(BOOLEAN)VmediaControlHasReply(c.Code),&c.LinuxErrno);
        if (NT_SUCCESS(status)) { RtlCopyMemory(out,&c,sizeof(c)); information=sizeof(c); }
    } else if (code==IOCTL_VGPU_VIDEO_BUFFERS) {
        VGPU_VIDEO_BUFFERS a=*(VGPU_VIDEO_BUFFERS *)in;
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(a),&out,NULL);
        if (!NT_SUCCESS(status)) goto unlock;
        a.LinuxErrno=0; status=VideoAllocate(d,&a);
        if (NT_SUCCESS(status)) { RtlCopyMemory(out,&a,sizeof(a)); information=sizeof(a); }
    } else if (code==IOCTL_VGPU_VIDEO_QUEUE) {
        status=VideoQueueBuffer(d,(VGPU_VIDEO_BUFFER *)in,inputBytes);
    } else if (code==IOCTL_VGPU_VIDEO_EVENT) {
        VGPU_VIDEO_EVENT event=*(VGPU_VIDEO_EVENT *)in;
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(event),&out,NULL);
        if (!NT_SUCCESS(status)) goto unlock;
        status=VideoGetEvent(d,&event);
        if (NT_SUCCESS(status)) { RtlCopyMemory(out,&event,sizeof(event)); information=sizeof(event); }
    } else if (code==IOCTL_VGPU_VIDEO_COPY) {
        VGPU_VIDEO_BUFFER r=*(VGPU_VIDEO_BUFFER *)in; VIDEO_BUFFER *b;
        if (r.Type!=VMEDIA_CAPTURE || r.Index>=d->Count[1]) { status=STATUS_INVALID_PARAMETER; goto unlock; }
        b=&d->Buffers[1][r.Index];
        if (b->Tracker.State!=VgpuBufferReturned) { status=STATUS_DEVICE_BUSY; goto unlock; }
        r.BytesUsed=b->Tracker.BytesUsed; r.Flags=b->Tracker.Flags; r.TimestampUs=b->Tracker.TimestampUs;
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(r)+r.BytesUsed,&out,NULL);
        if (!NT_SUCCESS(status)) goto unlock;
        RtlCopyMemory(out,&r,sizeof(r));
        KeMemoryBarrier();
        RtlCopyMemory((UCHAR *)out+sizeof(r),(UCHAR *)b->Address+b->Tracker.DataOffset,r.BytesUsed);
        information=sizeof(r)+r.BytesUsed;
    } else if (code==IOCTL_VGPU_VIDEO_STREAM) {
        VGPU_VIDEO_STREAM s=*(VGPU_VIDEO_STREAM *)in; ULONG type=s.Type; int q=VmediaQueueIndex(type),i;
        if (q<0 || s.On>1 || s.Reserved) { status=STATUS_INVALID_PARAMETER; goto unlock; }
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(s),&out,NULL);
        if (!NT_SUCCESS(status)) goto unlock;
        status=VideoControl(d,s.On?18:19,&type,sizeof(type),FALSE,&s.LinuxErrno);
        if (NT_SUCCESS(status) && !s.LinuxErrno) {
            d->Streaming[q]=(BOOLEAN)s.On;
            if (!s.On) {
                VideoPurgeEvents(d,q);
                for (i=0;i<(int)d->Count[q];++i) d->Buffers[q][i].Tracker.State=VgpuBufferIdle;
            }
        }
        if (NT_SUCCESS(status)) { RtlCopyMemory(out,&s,sizeof(s)); information=sizeof(s); }
    } else status=STATUS_INVALID_DEVICE_REQUEST;
unlock:
    WdfWaitLockRelease(d->SessionLock);
done:
    UNREFERENCED_PARAMETER(outputBytes);
    WdfRequestCompleteWithInformation(request,status,information);
}
/* Close on process exit. Failed CLOSE leaves allocations quarantined, not freed early. */
void VideoCleanup(WDFFILEOBJECT file)
{
    VIDEO_DEVICE *d=VideoGetDevice(WdfFileObjectGetDevice(file));
    WdfWaitLockAcquire(d->SessionLock,NULL);
    (void)VideoClose(d);
    WdfWaitLockRelease(d->SessionLock);
}
/* Initialize transport resources through the existing no-rdmapool VirtIO/WDF library. */
NTSTATUS VideoPrepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    VIDEO_DEVICE *d=VideoGetDevice(device); NTSTATUS status;
    UNREFERENCED_PARAMETER(raw);
    status=VirtIOWdfInitialize(&d->Virtio,device,translated,NULL,VIDEO_TAG);
    if (NT_SUCCESS(status)) d->Initialized=TRUE;
    return status;
}
/* Free transport allocations after queues are reset and no backend access remains. */
static void VideoFreeDma(VIDEO_DEVICE *d)
{
    ULONG i;
    if (d->Command) VirtIOWdfDeviceFreeDmaMemory(&d->Virtio.VIODevice,d->Command);
    if (d->Response) VirtIOWdfDeviceFreeDmaMemory(&d->Virtio.VIODevice,d->Response);
    d->Command=NULL; d->Response=NULL;
    for (i=0;i<VIDEO_EVENT_SLOTS;++i) {
        if (d->EventSlots[i].Address) VirtIOWdfDeviceFreeDmaMemory(&d->Virtio.VIODevice,d->EventSlots[i].Address);
        d->EventSlots[i].Address=NULL;
    }
}
/* Recreate both queues after every reset, with an already-populated event queue. */
NTSTATUS VideoD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    VIDEO_DEVICE *d=VideoGetDevice(device); VIRTIO_WDF_QUEUE_PARAM params[2];
    NTSTATUS status; ULONG i;
    UNREFERENCED_PARAMETER(previous);
    params[0].Interrupt=d->Interrupt; params[1].Interrupt=d->Interrupt;
    status=VirtIOWdfInitQueues(&d->Virtio,2,d->Queues,params);
    if (!NT_SUCCESS(status)) return status;
    d->Command=VirtIOWdfDeviceAllocDmaMemory(&d->Virtio.VIODevice,VIDEO_DMA_BYTES,VIDEO_TAG);
    d->Response=VirtIOWdfDeviceAllocDmaMemory(&d->Virtio.VIODevice,VIDEO_DMA_BYTES,VIDEO_TAG);
    if (!d->Command || !d->Response) { status=STATUS_INSUFFICIENT_RESOURCES; goto failed; }
    d->CommandDma=VirtIOWdfDeviceGetPhysicalAddress(&d->Virtio.VIODevice,d->Command);
    d->ResponseDma=VirtIOWdfDeviceGetPhysicalAddress(&d->Virtio.VIODevice,d->Response);
    for (i=0;i<VIDEO_EVENT_SLOTS;++i) {
        VIDEO_EVENT_SLOT *s=&d->EventSlots[i];
        s->Address=VirtIOWdfDeviceAllocDmaMemory(&d->Virtio.VIODevice,VIDEO_DMA_BYTES,VIDEO_TAG);
        if (!s->Address) { status=STATUS_INSUFFICIENT_RESOURCES; goto failed; }
        s->Dma=VirtIOWdfDeviceGetPhysicalAddress(&d->Virtio.VIODevice,s->Address);
        RtlZeroMemory(s->Address,VIDEO_DMA_BYTES);
        if (VideoPostEvent(d,s)<0) { status=STATUS_INSUFFICIENT_RESOURCES; goto failed; }
    }
    d->Poisoned=0; d->EventFailed=0; d->CommandPending=FALSE;
    d->EventHead=0; d->EventTail=0; d->EventCount=0;
    InterlockedExchange(&d->Ready,1);
    VirtIOWdfSetDriverOK(&d->Virtio);
    virtqueue_kick(d->Queues[1]);
    return STATUS_SUCCESS;
failed:
    VirtIOWdfSetDriverFailed(&d->Virtio); VirtIOWdfDestroyQueues(&d->Virtio);
    VideoFreeDma(d); return status;
}
/* D0 loss invalidates sessions; no decoder survives a transport reset by accident. */
NTSTATUS VideoD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
    VIDEO_DEVICE *d=VideoGetDevice(device);
    UNREFERENCED_PARAMETER(target);
    WdfWaitLockAcquire(d->SessionLock,NULL);
    WdfSpinLockAcquire(d->QueueLock); InterlockedExchange(&d->Ready,0); WdfSpinLockRelease(d->QueueLock);
    VirtIOWdfDestroyQueues(&d->Virtio);
    VideoFreeBuffers(d,0); VideoFreeBuffers(d,1); VideoFreeDma(d);
    d->Opened=FALSE; d->Streaming[0]=FALSE; d->Streaming[1]=FALSE; ++d->Generation;
    WdfWaitLockRelease(d->SessionLock);
    return STATUS_SUCCESS;
}
/* Tear down the transport mapping after D0Exit. */
NTSTATUS VideoRelease(WDFDEVICE device, WDFCMRESLIST resources)
{
    VIDEO_DEVICE *d=VideoGetDevice(device);
    UNREFERENCED_PARAMETER(resources);
    if (d->Initialized) { d->Initialized=FALSE; return VirtIOWdfShutdown(&d->Virtio); }
    return STATUS_SUCCESS;
}
/* Create an exclusive media endpoint; a decoder and an encoder have separate PCI functions. */
NTSTATUS VideoAdd(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    WDFDEVICE device; VIDEO_DEVICE *d; NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes; WDF_PNPPOWER_EVENT_CALLBACKS power;
    WDF_FILEOBJECT_CONFIG file; WDF_IO_QUEUE_CONFIG queue; WDF_INTERRUPT_CONFIG interrupt;
    UNREFERENCED_PARAMETER(driver);
    WdfDeviceInitSetDeviceType(init,FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(init,FILE_DEVICE_SECURE_OPEN,FALSE);
    WdfDeviceInitSetExclusive(init,TRUE);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDevicePrepareHardware=VideoPrepare; power.EvtDeviceReleaseHardware=VideoRelease;
    power.EvtDeviceD0Entry=VideoD0Entry; power.EvtDeviceD0Exit=VideoD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init,&power);
    WDF_FILEOBJECT_CONFIG_INIT(&file,WDF_NO_EVENT_CALLBACK,WDF_NO_EVENT_CALLBACK,VideoCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes); attributes.ExecutionLevel=WdfExecutionLevelPassive;
    attributes.SynchronizationScope=WdfSynchronizationScopeNone;
    WdfDeviceInitSetFileObjectConfig(init,&file,&attributes);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes,VIDEO_DEVICE);
    attributes.ExecutionLevel=WdfExecutionLevelPassive; attributes.SynchronizationScope=WdfSynchronizationScopeNone;
    status=WdfDeviceCreate(&init,&attributes,&device); if (!NT_SUCCESS(status)) return status;
    d=VideoGetDevice(device);
    KeInitializeEvent(&d->CommandDone,NotificationEvent,FALSE); KeInitializeEvent(&d->EventReady,NotificationEvent,FALSE);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes); attributes.ParentObject=device;
    status=WdfSpinLockCreate(&attributes,&d->QueueLock); if (!NT_SUCCESS(status)) return status;
    status=WdfWaitLockCreate(&attributes,&d->SessionLock); if (!NT_SUCCESS(status)) return status;
    WDF_INTERRUPT_CONFIG_INIT(&interrupt,VideoIsr,VideoDpc); interrupt.AutomaticSerialization=FALSE;
    status=WdfInterruptCreate(device,&interrupt,WDF_NO_OBJECT_ATTRIBUTES,&d->Interrupt); if (!NT_SUCCESS(status)) return status;
    status=WdfDeviceCreateDeviceInterface(device,&VideoInterface,NULL); if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue,WdfIoQueueDispatchSequential); queue.EvtIoDeviceControl=VideoIoctl;
    return WdfIoQueueCreate(device,&queue,WDF_NO_OBJECT_ATTRIBUTES,WDF_NO_HANDLE);
}
/* Register the VioGPU video companion driver. */
NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING path)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config,VideoAdd);
    return WdfDriverCreate(object,path,WDF_NO_OBJECT_ATTRIBUTES,&config,WDF_NO_HANDLE);
}
