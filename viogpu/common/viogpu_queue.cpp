/*
 * Copyright (C) 2019-2020 Red Hat, Inc.
 *
 * Written By: Vadim Rozenfeld <vrozenfe@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "viogpu_queue.h"
#include "baseobj.h"
#include "../../VirtIO/osdep.h"
#if !DBG
#include "viogpu_queue.tmh"
#endif

BOOLEAN VioGpuArmVbufferTerminalCallbacks(_Inout_ PGPU_VBUFFER buffer)
{
    if (buffer == NULL)
    {
        return FALSE;
    }
    KeClearEvent(&buffer->terminal_callback_event);
    BOOLEAN armed = InterlockedCompareExchange(&buffer->terminal_callback_state,
                                               VioGpuVbufferTerminalArmed,
                                               VioGpuVbufferTerminalUnarmed) == VioGpuVbufferTerminalUnarmed;
    if (!armed)
    {
        KeSetEvent(&buffer->terminal_callback_event, IO_NO_INCREMENT, FALSE);
    }
    return armed;
}

VIOGPU_VBUFFER_TERMINAL_CLAIM VioGpuClaimVbufferTerminalCallbacks(_Inout_ PGPU_VBUFFER buffer)
{
    if (buffer == NULL)
    {
        return VioGpuVbufferTerminalClaimLost;
    }

    LONG previous = InterlockedCompareExchange(&buffer->terminal_callback_state,
                                               VioGpuVbufferTerminalClaimed,
                                               VioGpuVbufferTerminalArmed);
    if (previous == VioGpuVbufferTerminalArmed)
    {
        return VioGpuVbufferTerminalClaimWon;
    }
    return previous == VioGpuVbufferTerminalUnarmed ? VioGpuVbufferTerminalClaimUnarmed
                                                    : VioGpuVbufferTerminalClaimLost;
}

VOID VioGpuDetachVbufferTerminalCallbacks(_Inout_ PGPU_VBUFFER buffer)
{
    if (buffer == NULL)
    {
        return;
    }
    buffer->complete_cb = NULL;
    buffer->complete_ctx = NULL;
    buffer->cancel_cb = NULL;
    buffer->cancel_ctx = NULL;
    buffer->queue_error_cb = NULL;
    buffer->queue_error_ctx = NULL;
}

VOID VioGpuCompleteVbufferTerminalCallbacks(_Inout_ PGPU_VBUFFER buffer)
{
    if (buffer == NULL)
    {
        return;
    }
    LONG state = InterlockedCompareExchange(&buffer->terminal_callback_state,
                                            VioGpuVbufferTerminalCompleted,
                                            VioGpuVbufferTerminalClaimed);
    if (state == VioGpuVbufferTerminalClaimed || state == VioGpuVbufferTerminalCompleted)
    {
        KeSetEvent(&buffer->terminal_callback_event, IO_NO_INCREMENT, FALSE);
    }
}

BOOLEAN VioGpuWaitForVbufferTerminalCallbacks(_Inout_ PGPU_VBUFFER buffer)
{
    if (buffer == NULL)
    {
        return FALSE;
    }
    LONG state = InterlockedCompareExchange(&buffer->terminal_callback_state, 0, 0);
    if (state != VioGpuVbufferTerminalClaimed)
    {
        return state == VioGpuVbufferTerminalCompleted || state == VioGpuVbufferTerminalUnarmed;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return FALSE;
    }

    LARGE_INTEGER timeout;
    timeout.QuadPart = -10LL * 10 * 1000 * 1000;
    return KeWaitForSingleObject(&buffer->terminal_callback_event,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 &timeout) == STATUS_SUCCESS &&
           InterlockedCompareExchange(&buffer->terminal_callback_state, 0, 0) == VioGpuVbufferTerminalCompleted;
}

static BOOLEAN BuildSGElement(VirtIOBufferDescriptor *sg, PVOID buf, ULONG size)
{
    if (size != 0 && MmIsAddressValid(buf))
    {
        sg->length = min(size, PAGE_SIZE - BYTE_OFFSET(buf));
        sg->physAddr = MmGetPhysicalAddress(buf);
        return sg->physAddr.QuadPart != 0;
    }
    return FALSE;
}

static void NotifyEventCompleteCB(void *ctx)
{
    KeSetEvent((PKEVENT)ctx, IO_NO_INCREMENT, FALSE);
}

VioGpuQueue::VioGpuQueue()
{
    m_pBuf = NULL;
    m_Index = (UINT)-1;
    m_pVIODevice = NULL;
    m_pVirtQueue = NULL;
    KeInitializeSpinLock(&m_SpinLock);
}

VioGpuQueue::~VioGpuQueue()
{
    Close();
}

void VioGpuQueue::Close(void)
{
    KIRQL SavedIrql;
    Lock(&SavedIrql);
    m_pVirtQueue = NULL;
    Unlock(SavedIrql);
}

BOOLEAN VioGpuQueue::Init(_In_ VirtIODevice *pVIODevice, _In_ struct virtqueue *pVirtQueue, _In_ UINT index)
{
    if ((pVIODevice == NULL) || (pVirtQueue == NULL))
    {
        return FALSE;
    }
    m_pVIODevice = pVIODevice;
    m_pVirtQueue = pVirtQueue;
    m_Index = index;
    return TRUE;
}

_IRQL_requires_max_(DISPATCH_LEVEL) _IRQL_saves_global_(OldIrql, Irql) _IRQL_raises_(DISPATCH_LEVEL) void VioGpuQueue::
                                                                                                    Lock(KIRQL *Irql)
{
    KIRQL SavedIrql = KeGetCurrentIrql();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s at IRQL %d\n", __FUNCTION__, SavedIrql));

    if (SavedIrql < DISPATCH_LEVEL)
    {
        KeAcquireSpinLock(&m_SpinLock, &SavedIrql);
    }
    else if (SavedIrql == DISPATCH_LEVEL)
    {
        KeAcquireSpinLockAtDpcLevel(&m_SpinLock);
    }
    else
    {
        // This is possible situation in case of bugcheck.
        // DxgkDdiSystemDisplayEnable can be called at any IRQL.
        // We need to allocate buffer for several command during this proccess.
        // VioGpuDbgBreak();
    }
    *Irql = SavedIrql;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

_IRQL_requires_(DISPATCH_LEVEL) _IRQL_restores_global_(OldIrql, Irql) void VioGpuQueue::Unlock(KIRQL Irql)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s at IRQL %d\n", __FUNCTION__, Irql));

    if (Irql < DISPATCH_LEVEL)
    {
        KeReleaseSpinLock(&m_SpinLock, Irql);
    }
    else if (Irql == DISPATCH_LEVEL)
    {
        KeReleaseSpinLockFromDpcLevel(&m_SpinLock);
    }
    else
    {
        // This is possible situation in case of bugcheck.
        // DxgkDdiSystemDisplayEnable can be called at any IRQL.
        // We need to allocate buffer for several command during this proccess.
        // VioGpuDbgBreak();
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

PAGED_CODE_SEG_BEGIN

UINT VioGpuQueue::QueryAllocation()
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    USHORT NumEntries;
    ULONG RingSize, HeapSize;

    NTSTATUS status = virtio_query_queue_allocation(m_pVIODevice, m_Index, &NumEntries, &RingSize, &HeapSize);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("[%s] virtio_query_queue_allocation(%d) failed with error %x\n", __FUNCTION__, m_Index, status));
        return 0;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return NumEntries;
}
PAGED_CODE_SEG_END

static LONG64 VioGpuMakeSynchronousEpochState(ULONG generation, VIOGPU_SYNCHRONOUS_STATE state)
{
    return static_cast<LONG64>((static_cast<ULONGLONG>(generation) << 32) | static_cast<ULONG>(state));
}

static ULONG VioGpuSynchronousGeneration(LONG64 epochState)
{
    return static_cast<ULONG>(static_cast<ULONGLONG>(epochState) >> 32);
}

static VIOGPU_SYNCHRONOUS_STATE VioGpuSynchronousState(LONG64 epochState)
{
    return static_cast<VIOGPU_SYNCHRONOUS_STATE>(static_cast<ULONG>(epochState));
}

static LONG64 VioGpuReadSynchronousEpochState(volatile LONG64 *epochState)
{
    return InterlockedCompareExchange64(epochState, 0, 0);
}

BOOLEAN CtrlQueue::IsSynchronousRequestsHealthy(void)
{
    return VioGpuSynchronousState(VioGpuReadSynchronousEpochState(&m_SynchronousEpochState)) ==
           VioGpuSynchronousEnabled;
}

void CtrlQueue::PoisonSynchronousRequests(void)
{
    LONG64 current = VioGpuReadSynchronousEpochState(&m_SynchronousEpochState);
    for (;;)
    {
        if (VioGpuSynchronousState(current) == VioGpuSynchronousPoisoned)
        {
            return;
        }
        ULONG generation = VioGpuSynchronousGeneration(current);
        if (generation != MAXULONG)
        {
            ++generation;
        }
        LONG64 poisoned = VioGpuMakeSynchronousEpochState(generation, VioGpuSynchronousPoisoned);
        LONG64 observed = InterlockedCompareExchange64(&m_SynchronousEpochState, poisoned, current);
        if (observed == current)
        {
            return;
        }
        current = observed;
    }
}

static BOOLEAN IsPlainControlResponse(PGPU_CTRL_HDR response, ULONG expectedType)
{
    return response != NULL && response->type == expectedType && response->flags == 0 && response->fence_id == 0 &&
           response->ctx_id == 0 && response->ring_idx == 0 && response->padding[0] == 0 && response->padding[1] == 0 &&
           response->padding[2] == 0;
}

static BOOLEAN IsPlainControlErrorResponse(PGPU_CTRL_HDR response)
{
    return response != NULL && response->type >= VIRTIO_GPU_RESP_ERR_UNSPEC &&
           response->type <= VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER && response->flags == 0 && response->fence_id == 0 &&
           response->ctx_id == 0 && response->ring_idx == 0 && response->padding[0] == 0 && response->padding[1] == 0 &&
           response->padding[2] == 0;
}

static BOOLEAN IsStandard2DResourceId(UINT resourceId)
{
    return resourceId != 0 && resourceId < VIOGPU_NATIVE_RESOURCE_ID_START;
}

static BOOLEAN IsSupported2DResourceFormat(UINT format)
{
    switch (format)
    {
        case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
        case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
        case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
        case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
            return TRUE;
        default:
            return FALSE;
    }
}

static BOOLEAN IsValid2DRectangle(UINT width, UINT height, UINT x, UINT y)
{
    return width != 0 && height != 0 && x <= MAXUINT - width && y <= MAXUINT - height;
}

BOOLEAN CtrlQueue::BeginSynchronousRequest(void)
{
    PAGED_CODE();

    if (KeGetCurrentIrql() != PASSIVE_LEVEL ||
        VioGpuSynchronousState(VioGpuReadSynchronousEpochState(&m_SynchronousEpochState)) != VioGpuSynchronousEnabled)
    {
        return FALSE;
    }

    LARGE_INTEGER timeout;
    timeout.QuadPart = -5LL * 10 * 1000 * 1000;
    NTSTATUS status = KeWaitForSingleObject(&m_SynchronousMutex, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
    {
        PoisonSynchronousRequests();
        return FALSE;
    }
    if (VioGpuSynchronousState(VioGpuReadSynchronousEpochState(&m_SynchronousEpochState)) != VioGpuSynchronousEnabled)
    {
        KeReleaseMutex(&m_SynchronousMutex, FALSE);
        return FALSE;
    }
    return TRUE;
}

void CtrlQueue::EndSynchronousRequest(void)
{
    PAGED_CODE();
    KeReleaseMutex(&m_SynchronousMutex, FALSE);
}

BOOLEAN CtrlQueue::EnableSynchronousRequests(void)
{
    PAGED_CODE();

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return FALSE;
    }
    LARGE_INTEGER timeout;
    timeout.QuadPart = -5LL * 10 * 1000 * 1000;
    NTSTATUS status = KeWaitForSingleObject(&m_SynchronousMutex, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
    {
        PoisonSynchronousRequests();
        return FALSE;
    }
    LONG64 current = VioGpuReadSynchronousEpochState(&m_SynchronousEpochState);
    BOOLEAN enabled = FALSE;
    if (VioGpuSynchronousState(current) == VioGpuSynchronousOffline)
    {
        ULONG generation = VioGpuSynchronousGeneration(current);
        if (generation != MAXULONG)
        {
            LONG64 next = VioGpuMakeSynchronousEpochState(generation + 1, VioGpuSynchronousEnabled);
            enabled = InterlockedCompareExchange64(&m_SynchronousEpochState, next, current) == current;
        }
    }
    KeReleaseMutex(&m_SynchronousMutex, FALSE);
    return enabled;
}

NTSTATUS CtrlQueue::QuiesceSynchronousRequests(void)
{
    PAGED_CODE();

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        PoisonSynchronousRequests();
        return STATUS_DEVICE_NOT_READY;
    }
    for (;;)
    {
        LONG64 current = VioGpuReadSynchronousEpochState(&m_SynchronousEpochState);
        VIOGPU_SYNCHRONOUS_STATE state = VioGpuSynchronousState(current);
        if (state == VioGpuSynchronousOffline)
        {
            return STATUS_SUCCESS;
        }
        if (state == VioGpuSynchronousQuiescing || state == VioGpuSynchronousPoisoned)
        {
            break;
        }
        if (state == VioGpuSynchronousEnabled)
        {
            LONG64 quiescing = VioGpuMakeSynchronousEpochState(VioGpuSynchronousGeneration(current),
                                                               VioGpuSynchronousQuiescing);
            if (InterlockedCompareExchange64(&m_SynchronousEpochState, quiescing, current) == current)
            {
                break;
            }
            continue;
        }
        PoisonSynchronousRequests();
        return STATUS_DEVICE_NOT_READY;
    }
    LARGE_INTEGER timeout;
    timeout.QuadPart = -6LL * 10 * 1000 * 1000;
    NTSTATUS status = KeWaitForSingleObject(&m_SynchronousMutex, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
    {
        PoisonSynchronousRequests();
        return status;
    }
    KeReleaseMutex(&m_SynchronousMutex, FALSE);
    return STATUS_SUCCESS;
}

void CtrlQueue::CompleteSynchronousRequestTeardown(void)
{
    PAGED_CODE();

    for (;;)
    {
        LONG64 current = VioGpuReadSynchronousEpochState(&m_SynchronousEpochState);
        VIOGPU_SYNCHRONOUS_STATE state = VioGpuSynchronousState(current);
        if (state == VioGpuSynchronousOffline)
        {
            return;
        }
        if (state != VioGpuSynchronousQuiescing && state != VioGpuSynchronousPoisoned)
        {
            NT_ASSERT(FALSE);
            return;
        }
        LONG64 offline = VioGpuMakeSynchronousEpochState(VioGpuSynchronousGeneration(current),
                                                         VioGpuSynchronousOffline);
        if (InterlockedCompareExchange64(&m_SynchronousEpochState, offline, current) == current)
        {
            return;
        }
    }
}

PAGED_CODE_SEG_BEGIN

BOOLEAN CtrlQueue::QueryDisplayInfo(UINT id, _Out_ PULONG xres, _Out_ PULONG yres)
{
    PAGED_CODE();

    if (xres == NULL || yres == NULL || id >= VIRTIO_GPU_MAX_SCANOUTS)
    {
        return FALSE;
    }
    *xres = 0;
    *yres = 0;
    if (!BeginSynchronousRequest())
    {
        return FALSE;
    }

    PGPU_RESP_DISP_INFO response = reinterpret_cast<PGPU_RESP_DISP_INFO>(m_pBuf->AllocateMemory(sizeof(GPU_RESP_DISP_INFO)));
    if (response == NULL)
    {
        EndSynchronousRequest();
        return FALSE;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_CTRL_HDR command = static_cast<PGPU_CTRL_HDR>(AllocCmdResp(&vbuf,
                                                                    sizeof(GPU_CTRL_HDR),
                                                                    response,
                                                                    sizeof(GPU_RESP_DISP_INFO)));
    if (command == NULL)
    {
        m_pBuf->FreeMemory(response);
        EndSynchronousRequest();
        return FALSE;
    }
    RtlZeroMemory(command, sizeof(*command));
    command->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    BOOLEAN releaseBuffer = TRUE;
    BOOLEAN success = SubmitSynchronousLocked(vbuf, &releaseBuffer) &&
                      vbuf->response_size == sizeof(GPU_RESP_DISP_INFO) &&
                      IsPlainControlResponse(&response->hdr, VIRTIO_GPU_RESP_OK_DISPLAY_INFO) &&
                      response->pmodes[id].enabled != 0;
    if (success)
    {
        *xres = response->pmodes[id].r.width;
        *yres = response->pmodes[id].r.height;
    }
    if (releaseBuffer)
    {
        ReleaseBuffer(vbuf);
    }
    EndSynchronousRequest();
    return success;
}

BOOLEAN CtrlQueue::QueryEdidInfo(UINT id, _Out_writes_bytes_(EDID_RAW_BLOCK_SIZE) PBYTE edid)
{
    PAGED_CODE();

    if (edid == NULL || id >= VIRTIO_GPU_MAX_SCANOUTS)
    {
        return FALSE;
    }
    RtlZeroMemory(edid, EDID_RAW_BLOCK_SIZE);
    if (!BeginSynchronousRequest())
    {
        return FALSE;
    }

    PGPU_RESP_EDID response = reinterpret_cast<PGPU_RESP_EDID>(m_pBuf->AllocateMemory(sizeof(GPU_RESP_EDID)));
    if (response == NULL)
    {
        EndSynchronousRequest();
        return FALSE;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_CMD_GET_EDID command = static_cast<PGPU_CMD_GET_EDID>(AllocCmdResp(&vbuf,
                                                                            sizeof(GPU_CMD_GET_EDID),
                                                                            response,
                                                                            sizeof(GPU_RESP_EDID)));
    if (command == NULL)
    {
        m_pBuf->FreeMemory(response);
        EndSynchronousRequest();
        return FALSE;
    }
    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_GET_EDID;
    command->scanout = id;

    BOOLEAN releaseBuffer = TRUE;
    BOOLEAN success = SubmitSynchronousLocked(vbuf, &releaseBuffer) && vbuf->response_size == sizeof(GPU_RESP_EDID) &&
                      IsPlainControlResponse(&response->hdr, VIRTIO_GPU_RESP_OK_EDID) && response->padding == 0 &&
                      response->size >= EDID_RAW_BLOCK_SIZE && response->size <= sizeof(response->edid);
    if (success)
    {
        RtlCopyMemory(edid, response->edid, EDID_RAW_BLOCK_SIZE);
    }
    if (releaseBuffer)
    {
        ReleaseBuffer(vbuf);
    }
    EndSynchronousRequest();
    return success;
}

void CtrlQueue::CreateResource(UINT res_id, UINT format, UINT width, UINT height)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_RES_CREATE_2D cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_CREATE_2D)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd->resource_id = res_id;
    cmd->format = format;
    cmd->width = width;
    cmd->height = height;

    // FIXME!!! if
    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void CtrlQueue::ResFlush(UINT res_id, UINT width, UINT height, UINT x, UINT y)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    PGPU_RES_FLUSH cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_FLUSH)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd->resource_id = res_id;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->r.x = x;
    cmd->r.y = y;

    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void CtrlQueue::TransferToHost2D(UINT res_id, ULONG offset, UINT width, UINT height, UINT x, UINT y)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    PGPU_RES_TRANSF_TO_HOST_2D cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_TRANSF_TO_HOST_2D)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd->resource_id = res_id;
    cmd->offset = offset;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->r.x = x;
    cmd->r.y = y;

    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void CtrlQueue::AttachBacking(UINT res_id, PGPU_MEM_ENTRY ents, UINT nents)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_RES_ATTACH_BACKING cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_ATTACH_BACKING)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd->resource_id = res_id;
    cmd->nr_entries = nents;

    vbuf->data_buf = ents;
    vbuf->data_size = sizeof(*ents) * nents;

    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

PAGED_CODE_SEG_END

BOOLEAN CtrlQueue::SubmitSynchronousLocked(PGPU_VBUFFER buf, _Out_ PBOOLEAN release_buffer)
{
    BOOLEAN submitted = FALSE;
    return SubmitSynchronousLocked(buf, release_buffer, &submitted);
}

BOOLEAN CtrlQueue::SubmitSynchronousLocked(PGPU_VBUFFER buf, _Out_ PBOOLEAN release_buffer, _Out_ PBOOLEAN submitted)
{
    if (buf == NULL || release_buffer == NULL || submitted == NULL)
    {
        return FALSE;
    }

    *release_buffer = TRUE;
    *submitted = FALSE;
    LONG64 requestEpochState = VioGpuReadSynchronousEpochState(&m_SynchronousEpochState);
    if (VioGpuSynchronousState(requestEpochState) != VioGpuSynchronousEnabled)
    {
        return FALSE;
    }
    KeClearEvent(&buf->completion_event);
    buf->synchronous_epoch_state = requestEpochState;
    buf->complete_cb = NotifyEventCompleteCB;
    buf->complete_ctx = &buf->completion_event;
    buf->auto_release = false;

    if (QueueBuffer(buf) < 0)
    {
        buf->complete_cb = NULL;
        buf->complete_ctx = NULL;
        buf->synchronous_epoch_state = 0;
        return FALSE;
    }
    *submitted = TRUE;

    LARGE_INTEGER timeout;
    timeout.QuadPart = -5LL * 10 * 1000 * 1000;
    NTSTATUS status = KeWaitForSingleObject(&buf->completion_event, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
    {
        // The device still owns the descriptor. The adapter reset path reclaims
        // it only after interrupts are disabled and queued DPCs are flushed.
        PoisonSynchronousRequests();
        *release_buffer = FALSE;
        DbgPrint(TRACE_LEVEL_ERROR, ("%s timed out with status 0x%x\n", __FUNCTION__, status));
        return FALSE;
    }
    LONG64 completedEpochState = VioGpuReadSynchronousEpochState(&m_SynchronousEpochState);
    if (completedEpochState != requestEpochState || buf->synchronous_epoch_state != requestEpochState ||
        VioGpuSynchronousState(completedEpochState) != VioGpuSynchronousEnabled)
    {
        // A reset or quiesce raced the completion callback. Even if the DPC
        // dequeued this descriptor, retain it until reset reclamation proves
        // that no host or callback can still reference its storage.
        *release_buffer = FALSE;
        return FALSE;
    }
    return TRUE;
}

VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::SubmitSynchronousNoDataLocked(PGPU_VBUFFER buf)
{
    if (buf == NULL)
    {
        return VioGpuHostContextNotSubmitted;
    }

    BOOLEAN releaseBuffer = TRUE;
    BOOLEAN submitted = FALSE;
    BOOLEAN completed = SubmitSynchronousLocked(buf, &releaseBuffer, &submitted);
    PGPU_CTRL_HDR response = reinterpret_cast<PGPU_CTRL_HDR>(buf->resp_buf);
    VIOGPU_HOST_CONTEXT_RESULT result = VioGpuHostContextUnknown;
    if (!submitted)
    {
        result = VioGpuHostContextNotSubmitted;
    }
    else if (completed && buf->response_size == sizeof(GPU_CTRL_HDR))
    {
        if (IsPlainControlResponse(response, VIRTIO_GPU_RESP_OK_NODATA))
        {
            result = VioGpuHostContextConfirmed;
        }
        else if (IsPlainControlErrorResponse(response))
        {
            result = VioGpuHostContextRejected;
        }
        else
        {
            PoisonSynchronousRequests();
        }
    }
    else if (completed)
    {
        PoisonSynchronousRequests();
    }
    if (releaseBuffer)
    {
        ReleaseBuffer(buf);
    }
    return result;
}

PAGED_CODE_SEG_BEGIN

VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::CreateResource2DSynchronous(UINT resource_id,
                                                                  UINT format,
                                                                  UINT width,
                                                                  UINT height)
{
    PAGED_CODE();

    if (!IsStandard2DResourceId(resource_id) || !IsSupported2DResourceFormat(format) || width == 0 || height == 0 ||
        !BeginSynchronousRequest())
    {
        return VioGpuHostContextNotSubmitted;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_RES_CREATE_2D command = static_cast<PGPU_RES_CREATE_2D>(AllocCmd(&vbuf, sizeof(*command)));
    if (command == NULL)
    {
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }
    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    command->resource_id = resource_id;
    command->format = format;
    command->width = width;
    command->height = height;

    VIOGPU_HOST_CONTEXT_RESULT result = SubmitSynchronousNoDataLocked(vbuf);
    EndSynchronousRequest();
    return result;
}

VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::AttachBackingSynchronous(UINT resource_id,
                                                               const GPU_MEM_ENTRY *entries,
                                                               UINT entry_count)
{
    PAGED_CODE();

    if (!IsStandard2DResourceId(resource_id) || entries == NULL || entry_count == 0 ||
        entry_count > VIOGPU_MAX_BACKING_ENTRIES)
    {
        return VioGpuHostContextNotSubmitted;
    }
    for (UINT index = 0; index < entry_count; ++index)
    {
        if (entries[index].addr == 0 || entries[index].length == 0 || entries[index].padding != 0 ||
            entries[index].addr > MAXULONGLONG - (entries[index].length - 1))
        {
            return VioGpuHostContextNotSubmitted;
        }
    }
    if (!BeginSynchronousRequest())
    {
        return VioGpuHostContextNotSubmitted;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_RES_ATTACH_BACKING command = static_cast<PGPU_RES_ATTACH_BACKING>(AllocCmd(&vbuf, sizeof(*command)));
    if (command == NULL)
    {
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }
    SIZE_T entriesSize = sizeof(*entries) * (SIZE_T)entry_count;
    PGPU_MEM_ENTRY ownedEntries = static_cast<PGPU_MEM_ENTRY>(m_pBuf->AllocateMemory(entriesSize));
    if (ownedEntries == NULL)
    {
        ReleaseBuffer(vbuf);
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }
    RtlCopyMemory(ownedEntries, entries, entriesSize);

    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    command->resource_id = resource_id;
    command->nr_entries = entry_count;
    vbuf->data_buf = ownedEntries;
    vbuf->data_size = (UINT)entriesSize;

    VIOGPU_HOST_CONTEXT_RESULT result = SubmitSynchronousNoDataLocked(vbuf);
    EndSynchronousRequest();
    return result;
}

VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::DetachBackingSynchronous(UINT resource_id)
{
    PAGED_CODE();

    if (!IsStandard2DResourceId(resource_id) || !BeginSynchronousRequest())
    {
        return VioGpuHostContextNotSubmitted;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_RES_DETACH_BACKING command = static_cast<PGPU_RES_DETACH_BACKING>(AllocCmd(&vbuf, sizeof(*command)));
    if (command == NULL)
    {
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }
    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    command->resource_id = resource_id;

    VIOGPU_HOST_CONTEXT_RESULT result = SubmitSynchronousNoDataLocked(vbuf);
    EndSynchronousRequest();
    return result;
}

VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::UnrefResourceSynchronous(UINT resource_id)
{
    PAGED_CODE();

    if (!IsStandard2DResourceId(resource_id) || !BeginSynchronousRequest())
    {
        return VioGpuHostContextNotSubmitted;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_RES_UNREF command = static_cast<PGPU_RES_UNREF>(AllocCmd(&vbuf, sizeof(*command)));
    if (command == NULL)
    {
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }
    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    command->resource_id = resource_id;

    VIOGPU_HOST_CONTEXT_RESULT result = SubmitSynchronousNoDataLocked(vbuf);
    EndSynchronousRequest();
    return result;
}

PAGED_CODE_SEG_END

VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::SetScanoutSynchronous(UINT scanout_id,
                                                            UINT resource_id,
                                                            UINT width,
                                                            UINT height,
                                                            UINT x,
                                                            UINT y)
{
    BOOLEAN disable = resource_id == 0 && width == 0 && height == 0 && x == 0 && y == 0;
    if (scanout_id >= VIRTIO_GPU_MAX_SCANOUTS ||
        (!disable && (!IsStandard2DResourceId(resource_id) || !IsValid2DRectangle(width, height, x, y))) ||
        !BeginSynchronousRequest())
    {
        return VioGpuHostContextNotSubmitted;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_SET_SCANOUT command = static_cast<PGPU_SET_SCANOUT>(AllocCmd(&vbuf, sizeof(*command)));
    if (command == NULL)
    {
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }
    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    command->resource_id = resource_id;
    command->scanout_id = scanout_id;
    command->r.width = width;
    command->r.height = height;
    command->r.x = x;
    command->r.y = y;

    VIOGPU_HOST_CONTEXT_RESULT result = SubmitSynchronousNoDataLocked(vbuf);
    EndSynchronousRequest();
    return result;
}

PAGED_CODE_SEG_BEGIN

VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::TransferToHost2DSynchronous(UINT resource_id,
                                                                  ULONGLONG offset,
                                                                  UINT width,
                                                                  UINT height,
                                                                  UINT x,
                                                                  UINT y)
{
    PAGED_CODE();

    if (!IsStandard2DResourceId(resource_id) || !IsValid2DRectangle(width, height, x, y) || !BeginSynchronousRequest())
    {
        return VioGpuHostContextNotSubmitted;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_RES_TRANSF_TO_HOST_2D command = static_cast<PGPU_RES_TRANSF_TO_HOST_2D>(AllocCmd(&vbuf, sizeof(*command)));
    if (command == NULL)
    {
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }
    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    command->resource_id = resource_id;
    command->offset = offset;
    command->r.width = width;
    command->r.height = height;
    command->r.x = x;
    command->r.y = y;

    VIOGPU_HOST_CONTEXT_RESULT result = SubmitSynchronousNoDataLocked(vbuf);
    EndSynchronousRequest();
    return result;
}

VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::FlushResourceSynchronous(UINT resource_id,
                                                               UINT width,
                                                               UINT height,
                                                               UINT x,
                                                               UINT y)
{
    PAGED_CODE();

    if (!IsStandard2DResourceId(resource_id) || !IsValid2DRectangle(width, height, x, y) || !BeginSynchronousRequest())
    {
        return VioGpuHostContextNotSubmitted;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_RES_FLUSH command = static_cast<PGPU_RES_FLUSH>(AllocCmd(&vbuf, sizeof(*command)));
    if (command == NULL)
    {
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }
    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    command->resource_id = resource_id;
    command->r.width = width;
    command->r.height = height;
    command->r.x = x;
    command->r.y = y;

    VIOGPU_HOST_CONTEXT_RESULT result = SubmitSynchronousNoDataLocked(vbuf);
    EndSynchronousRequest();
    return result;
}

BOOLEAN CtrlQueue::QueryCapsetInfo(UINT capset_index, PGPU_RESP_CAPSET_INFO capset_info)
{
    PAGED_CODE();

    if (capset_info == NULL || m_pBuf == NULL)
    {
        return FALSE;
    }
    RtlZeroMemory(capset_info, sizeof(*capset_info));
    if (!BeginSynchronousRequest())
    {
        return FALSE;
    }

    PGPU_RESP_CAPSET_INFO response = static_cast<PGPU_RESP_CAPSET_INFO>(m_pBuf->AllocateMemory(sizeof(GPU_RESP_CAPSET_INFO)));
    if (response == NULL)
    {
        EndSynchronousRequest();
        return FALSE;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_CMD_GET_CAPSET_INFO command = static_cast<PGPU_CMD_GET_CAPSET_INFO>(AllocCmdResp(&vbuf,
                                                                                          sizeof(GPU_CMD_GET_CAPSET_INFO),
                                                                                          response,
                                                                                          sizeof(GPU_RESP_CAPSET_INFO)));
    if (command == NULL)
    {
        m_pBuf->FreeMemory(response);
        EndSynchronousRequest();
        return FALSE;
    }

    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
    command->capset_index = capset_index;

    BOOLEAN releaseBuffer = TRUE;
    BOOLEAN success = SubmitSynchronousLocked(vbuf, &releaseBuffer) &&
                      vbuf->response_size == sizeof(GPU_RESP_CAPSET_INFO) &&
                      IsPlainControlResponse(&response->hdr, VIRTIO_GPU_RESP_OK_CAPSET_INFO) && response->padding == 0;
    if (success)
    {
        *capset_info = *response;
    }
    if (releaseBuffer)
    {
        ReleaseBuffer(vbuf);
    }
    EndSynchronousRequest();
    return success;
}

BOOLEAN CtrlQueue::QueryCapset(UINT capset_id, UINT capset_version, UINT capset_size, PGPU_CAPSET_DRM capset)
{
    PAGED_CODE();

    const UINT requiredSize = FIELD_OFFSET(GPU_CAPSET_DRM, msm.va_size) + sizeof(capset->msm.va_size);
    if (capset == NULL || m_pBuf == NULL || capset_size < requiredSize ||
        capset_size > PAGE_SIZE - sizeof(GPU_CTRL_HDR))
    {
        return FALSE;
    }
    RtlZeroMemory(capset, sizeof(*capset));
    if (!BeginSynchronousRequest())
    {
        return FALSE;
    }

    UINT responseSize = sizeof(GPU_CTRL_HDR) + capset_size;
    PGPU_CTRL_HDR response = static_cast<PGPU_CTRL_HDR>(m_pBuf->AllocateMemory(responseSize));
    if (response == NULL)
    {
        EndSynchronousRequest();
        return FALSE;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_CMD_GET_CAPSET command = static_cast<PGPU_CMD_GET_CAPSET>(AllocCmdResp(&vbuf,
                                                                                sizeof(GPU_CMD_GET_CAPSET),
                                                                                response,
                                                                                responseSize));
    if (command == NULL)
    {
        m_pBuf->FreeMemory(response);
        EndSynchronousRequest();
        return FALSE;
    }

    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    command->capset_id = capset_id;
    command->capset_version = capset_version;

    BOOLEAN releaseBuffer = TRUE;
    BOOLEAN success = SubmitSynchronousLocked(vbuf, &releaseBuffer) && vbuf->response_size == responseSize &&
                      IsPlainControlResponse(response, VIRTIO_GPU_RESP_OK_CAPSET);
    if (success)
    {
        RtlZeroMemory(capset, sizeof(*capset));
        RtlCopyMemory(capset,
                      reinterpret_cast<PUCHAR>(response) + sizeof(*response),
                      min((SIZE_T)capset_size, sizeof(*capset)));
    }
    if (releaseBuffer)
    {
        ReleaseBuffer(vbuf);
    }
    EndSynchronousRequest();
    return success;
}

void CtrlQueue::GetLastNativeContextResponseDiagnostic(_Out_ PVIOGPU_HOST_CONTEXT_RESPONSE_DIAGNOSTIC diagnostic) const
{
    if (diagnostic != NULL)
    {
        RtlCopyMemory(diagnostic, &m_LastNativeContextResponseDiagnostic, sizeof(*diagnostic));
    }
}

VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::CreateNativeContext(UINT context_id,
                                                          _Out_opt_ PVIOGPU_HOST_CONTEXT_RESPONSE_DIAGNOSTIC diagnostic)
{
    PAGED_CODE();

    VIOGPU_HOST_CONTEXT_RESPONSE_DIAGNOSTIC captured = {};
    PVIOGPU_HOST_CONTEXT_RESPONSE_DIAGNOSTIC output = diagnostic != NULL ? diagnostic : &captured;
    RtlZeroMemory(&m_LastNativeContextResponseDiagnostic, sizeof(m_LastNativeContextResponseDiagnostic));
    RtlZeroMemory(output, sizeof(*output));

    if (context_id == 0)
    {
        return VioGpuHostContextNotSubmitted;
    }
    if (!BeginSynchronousRequest())
    {
        return VioGpuHostContextNotSubmitted;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_CMD_CTX_CREATE command = static_cast<PGPU_CMD_CTX_CREATE>(AllocCmd(&vbuf, sizeof(GPU_CMD_CTX_CREATE)));
    if (command == NULL)
    {
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }

    static const UCHAR contextName[] = "viogpu-wddm";
    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    command->hdr.ctx_id = context_id;
    command->nlen = sizeof(contextName) - 1;
    command->context_init = VIRTIO_GPU_CAPSET_DRM & VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK;
    RtlCopyMemory(command->debug_name, contextName, sizeof(contextName) - 1);

    BOOLEAN releaseBuffer = TRUE;
    BOOLEAN submitted = FALSE;
    BOOLEAN completed = SubmitSynchronousLocked(vbuf, &releaseBuffer, &submitted);
    PGPU_CTRL_HDR response = reinterpret_cast<PGPU_CTRL_HDR>(vbuf->resp_buf);
    output->ResponseSize = vbuf->response_size;
    output->Submitted = submitted;
    output->Completed = completed;
    if (completed && vbuf->response_size >= sizeof(GPU_CTRL_HDR) && response != NULL)
    {
        output->Type = response->type;
        output->Flags = response->flags;
        output->FenceId = response->fence_id;
        output->ContextId = response->ctx_id;
        output->RingIndex = response->ring_idx;
        RtlCopyMemory(output->Padding, response->padding, sizeof(output->Padding));
    }
    RtlCopyMemory(&m_LastNativeContextResponseDiagnostic, output, sizeof(m_LastNativeContextResponseDiagnostic));
    VIOGPU_HOST_CONTEXT_RESULT result = VioGpuHostContextUnknown;
    if (!submitted)
    {
        result = VioGpuHostContextNotSubmitted;
    }
    else if (completed && vbuf->response_size == sizeof(GPU_CTRL_HDR))
    {
        if (IsPlainControlResponse(response, VIRTIO_GPU_RESP_OK_NODATA))
        {
            result = VioGpuHostContextConfirmed;
        }
        else if (IsPlainControlErrorResponse(response))
        {
            result = VioGpuHostContextRejected;
        }
    }
    if (releaseBuffer)
    {
        ReleaseBuffer(vbuf);
    }
    EndSynchronousRequest();
    return result;
}

VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::DestroyNativeContext(UINT context_id)
{
    PAGED_CODE();

    if (context_id == 0)
    {
        return VioGpuHostContextNotSubmitted;
    }
    if (!BeginSynchronousRequest())
    {
        return VioGpuHostContextNotSubmitted;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_CMD_CTX_DESTROY command = static_cast<PGPU_CMD_CTX_DESTROY>(AllocCmd(&vbuf, sizeof(GPU_CMD_CTX_DESTROY)));
    if (command == NULL)
    {
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }

    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    command->hdr.ctx_id = context_id;

    BOOLEAN releaseBuffer = TRUE;
    BOOLEAN submitted = FALSE;
    BOOLEAN completed = SubmitSynchronousLocked(vbuf, &releaseBuffer, &submitted);
    PGPU_CTRL_HDR response = reinterpret_cast<PGPU_CTRL_HDR>(vbuf->resp_buf);
    VIOGPU_HOST_CONTEXT_RESULT result = VioGpuHostContextUnknown;
    if (!submitted)
    {
        result = VioGpuHostContextNotSubmitted;
    }
    else if (completed && vbuf->response_size == sizeof(GPU_CTRL_HDR))
    {
        if (IsPlainControlResponse(response, VIRTIO_GPU_RESP_OK_NODATA))
        {
            result = VioGpuHostContextConfirmed;
        }
        else if (IsPlainControlErrorResponse(response) && response->type == VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID)
        {
            result = VioGpuHostContextRejected;
        }
    }
    if (releaseBuffer)
    {
        ReleaseBuffer(vbuf);
    }
    EndSynchronousRequest();
    return result;
}

VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::CreateNativeControlBlob(UINT context_id, UINT resource_id)
{
    PAGED_CODE();

    if (context_id == 0 || resource_id == 0 || !BeginSynchronousRequest())
    {
        return VioGpuHostContextNotSubmitted;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_CMD_RESOURCE_CREATE_BLOB command = static_cast<PGPU_CMD_RESOURCE_CREATE_BLOB>(AllocCmd(&vbuf,
                                                                                                sizeof(GPU_CMD_RESOURCE_CREATE_BLOB)));
    if (command == NULL)
    {
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }

    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    command->hdr.ctx_id = context_id;
    command->resource_id = resource_id;
    command->blob_mem = VIRTIO_GPU_BLOB_MEM_HOST3D;
    command->blob_flags = VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE;
    command->blob_id = 0;
    command->size = VIOGPU_NATIVE_CONTROL_BLOB_SIZE;

    BOOLEAN releaseBuffer = TRUE;
    BOOLEAN submitted = FALSE;
    BOOLEAN completed = SubmitSynchronousLocked(vbuf, &releaseBuffer, &submitted);
    PGPU_CTRL_HDR response = reinterpret_cast<PGPU_CTRL_HDR>(vbuf->resp_buf);
    VIOGPU_HOST_CONTEXT_RESULT result = VioGpuHostContextUnknown;
    if (!submitted)
    {
        result = VioGpuHostContextNotSubmitted;
    }
    else if (completed && vbuf->response_size == sizeof(GPU_CTRL_HDR))
    {
        if (IsPlainControlResponse(response, VIRTIO_GPU_RESP_OK_NODATA))
        {
            result = VioGpuHostContextConfirmed;
        }
        else if (IsPlainControlErrorResponse(response))
        {
            result = VioGpuHostContextRejected;
        }
        else
        {
            PoisonSynchronousRequests();
        }
    }
    else if (completed)
    {
        PoisonSynchronousRequests();
    }
    if (releaseBuffer)
    {
        ReleaseBuffer(vbuf);
    }
    EndSynchronousRequest();
    return result;
}

VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::CreateNativeGuestBlob(UINT context_id,
                                                            UINT resource_id,
                                                            UINT blob_id,
                                                            ULONGLONG size,
                                                            UINT blob_flags,
                                                            const GPU_MEM_ENTRY *entries,
                                                            UINT entry_count)
{
    PAGED_CODE();

    const UINT validBlobFlags = VIRTIO_GPU_BLOB_FLAG_CREATE_GUEST_HANDLE | VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE;
    if (context_id == 0 || resource_id < VIOGPU_NATIVE_RESOURCE_ID_START || blob_id == 0 || size == 0 ||
        size > MAXULONG || (size & (PAGE_SIZE - 1)) != 0 ||
        (blob_flags & VIRTIO_GPU_BLOB_FLAG_CREATE_GUEST_HANDLE) == 0 || (blob_flags & ~validBlobFlags) != 0 ||
        entries == NULL || entry_count == 0 || entry_count > VIOGPU_MAX_BACKING_ENTRIES ||
        entry_count > MAXULONG / sizeof(GPU_MEM_ENTRY) || !BeginSynchronousRequest())
    {
        return VioGpuHostContextNotSubmitted;
    }

    ULONGLONG entryBytes = 0;
    for (UINT index = 0; index < entry_count; ++index)
    {
        if ((entries[index].addr & (PAGE_SIZE - 1)) != 0 || entries[index].length == 0 ||
            (entries[index].length & (PAGE_SIZE - 1)) != 0 || entries[index].padding != 0 || entryBytes > size ||
            entries[index].length > size - entryBytes)
        {
            EndSynchronousRequest();
            return VioGpuHostContextNotSubmitted;
        }
        entryBytes += entries[index].length;
    }
    if (entryBytes != size)
    {
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_CMD_RESOURCE_CREATE_BLOB command = static_cast<PGPU_CMD_RESOURCE_CREATE_BLOB>(AllocCmd(&vbuf,
                                                                                                sizeof(*command)));
    if (command == NULL)
    {
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }

    SIZE_T entriesSize = static_cast<SIZE_T>(entry_count) * sizeof(GPU_MEM_ENTRY);
    PGPU_MEM_ENTRY ownedEntries = static_cast<PGPU_MEM_ENTRY>(m_pBuf->AllocateMemory(entriesSize));
    if (ownedEntries == NULL)
    {
        ReleaseBuffer(vbuf);
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }
    RtlCopyMemory(ownedEntries, entries, entriesSize);

    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    command->hdr.ctx_id = context_id;
    command->resource_id = resource_id;
    command->blob_mem = VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST;
    command->blob_flags = blob_flags;
    command->nr_entries = entry_count;
    command->blob_id = blob_id;
    command->size = size;
    vbuf->data_buf = ownedEntries;
    vbuf->data_size = static_cast<ULONG>(entriesSize);

    BOOLEAN releaseBuffer = TRUE;
    BOOLEAN submitted = FALSE;
    BOOLEAN completed = SubmitSynchronousLocked(vbuf, &releaseBuffer, &submitted);
    PGPU_CTRL_HDR response = reinterpret_cast<PGPU_CTRL_HDR>(vbuf->resp_buf);
    VIOGPU_HOST_CONTEXT_RESULT result = VioGpuHostContextUnknown;
    if (!submitted)
    {
        result = VioGpuHostContextNotSubmitted;
    }
    else if (completed && vbuf->response_size == sizeof(GPU_CTRL_HDR))
    {
        if (IsPlainControlResponse(response, VIRTIO_GPU_RESP_OK_NODATA))
        {
            result = VioGpuHostContextConfirmed;
        }
        else if (IsPlainControlErrorResponse(response))
        {
            result = VioGpuHostContextRejected;
        }
        else
        {
            PoisonSynchronousRequests();
        }
    }
    else if (completed)
    {
        PoisonSynchronousRequests();
    }
    if (releaseBuffer)
    {
        ReleaseBuffer(vbuf);
    }
    EndSynchronousRequest();
    return result;
}

VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::MapNativeControlBlob(UINT resource_id, ULONGLONG offset)
{
    PAGED_CODE();

    if (resource_id == 0 || (offset & (PAGE_SIZE - 1)) != 0 || !BeginSynchronousRequest())
    {
        return VioGpuHostContextNotSubmitted;
    }

    PGPU_RESP_MAP_INFO response = static_cast<PGPU_RESP_MAP_INFO>(m_pBuf->AllocateMemory(sizeof(GPU_RESP_MAP_INFO)));
    if (response == NULL)
    {
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_CMD_RESOURCE_MAP_BLOB command = static_cast<PGPU_CMD_RESOURCE_MAP_BLOB>(AllocCmdResp(&vbuf,
                                                                                              sizeof(GPU_CMD_RESOURCE_MAP_BLOB),
                                                                                              response,
                                                                                              sizeof(GPU_RESP_MAP_INFO)));
    if (command == NULL)
    {
        m_pBuf->FreeMemory(response);
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }

    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    command->resource_id = resource_id;
    command->offset = offset;

    BOOLEAN releaseBuffer = TRUE;
    BOOLEAN submitted = FALSE;
    BOOLEAN completed = SubmitSynchronousLocked(vbuf, &releaseBuffer, &submitted);
    VIOGPU_HOST_CONTEXT_RESULT result = VioGpuHostContextUnknown;
    if (!submitted)
    {
        result = VioGpuHostContextNotSubmitted;
    }
    else if (completed && vbuf->response_size == sizeof(GPU_RESP_MAP_INFO) &&
             IsPlainControlResponse(&response->hdr, VIRTIO_GPU_RESP_OK_MAP_INFO) &&
             response->map_info == VIRTIO_GPU_MAP_CACHE_CACHED && response->padding == 0)
    {
        result = VioGpuHostContextConfirmed;
    }
    else if (completed && vbuf->response_size == sizeof(GPU_CTRL_HDR) && IsPlainControlErrorResponse(&response->hdr))
    {
        result = VioGpuHostContextRejected;
    }
    else if (completed)
    {
        PoisonSynchronousRequests();
    }
    if (releaseBuffer)
    {
        ReleaseBuffer(vbuf);
    }
    EndSynchronousRequest();
    return result;
}

VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::UnmapNativeControlBlob(UINT resource_id)
{
    PAGED_CODE();

    if (resource_id == 0 || !BeginSynchronousRequest())
    {
        return VioGpuHostContextNotSubmitted;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_CMD_RESOURCE_UNMAP_BLOB command = static_cast<PGPU_CMD_RESOURCE_UNMAP_BLOB>(AllocCmd(&vbuf,
                                                                                              sizeof(GPU_CMD_RESOURCE_UNMAP_BLOB)));
    if (command == NULL)
    {
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }

    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB;
    command->resource_id = resource_id;

    BOOLEAN releaseBuffer = TRUE;
    BOOLEAN submitted = FALSE;
    BOOLEAN completed = SubmitSynchronousLocked(vbuf, &releaseBuffer, &submitted);
    PGPU_CTRL_HDR response = reinterpret_cast<PGPU_CTRL_HDR>(vbuf->resp_buf);
    VIOGPU_HOST_CONTEXT_RESULT result = VioGpuHostContextUnknown;
    if (!submitted)
    {
        result = VioGpuHostContextNotSubmitted;
    }
    else if (completed && vbuf->response_size == sizeof(GPU_CTRL_HDR) &&
             IsPlainControlResponse(response, VIRTIO_GPU_RESP_OK_NODATA))
    {
        result = VioGpuHostContextConfirmed;
    }
    else if (completed && vbuf->response_size == sizeof(GPU_CTRL_HDR) && IsPlainControlErrorResponse(response) &&
             response->type == VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID)
    {
        result = VioGpuHostContextRejected;
    }
    else if (completed)
    {
        PoisonSynchronousRequests();
    }
    if (releaseBuffer)
    {
        ReleaseBuffer(vbuf);
    }
    EndSynchronousRequest();
    return result;
}

VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::UnrefNativeResource(UINT resource_id)
{
    PAGED_CODE();

    if (resource_id == 0 || !BeginSynchronousRequest())
    {
        return VioGpuHostContextNotSubmitted;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_RES_UNREF command = static_cast<PGPU_RES_UNREF>(AllocCmd(&vbuf, sizeof(GPU_RES_UNREF)));
    if (command == NULL)
    {
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }

    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    command->resource_id = resource_id;

    BOOLEAN releaseBuffer = TRUE;
    BOOLEAN submitted = FALSE;
    BOOLEAN completed = SubmitSynchronousLocked(vbuf, &releaseBuffer, &submitted);
    PGPU_CTRL_HDR response = reinterpret_cast<PGPU_CTRL_HDR>(vbuf->resp_buf);
    VIOGPU_HOST_CONTEXT_RESULT result = VioGpuHostContextUnknown;
    if (!submitted)
    {
        result = VioGpuHostContextNotSubmitted;
    }
    else if (completed && vbuf->response_size == sizeof(GPU_CTRL_HDR) &&
             IsPlainControlResponse(response, VIRTIO_GPU_RESP_OK_NODATA))
    {
        result = VioGpuHostContextConfirmed;
    }
    else if (completed && vbuf->response_size == sizeof(GPU_CTRL_HDR) && IsPlainControlErrorResponse(response) &&
             response->type == VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID)
    {
        result = VioGpuHostContextRejected;
    }
    else if (completed)
    {
        PoisonSynchronousRequests();
    }
    if (releaseBuffer)
    {
        ReleaseBuffer(vbuf);
    }
    EndSynchronousRequest();
    return result;
}

VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::SubmitNativeControl(UINT context_id, const void *command, UINT command_size)
{
    PAGED_CODE();

    if (context_id == 0 || command == NULL || command_size == 0 || !BeginSynchronousRequest())
    {
        return VioGpuHostContextNotSubmitted;
    }

    PGPU_VBUFFER vbuf = PrepareNativeSubmit(context_id, command, command_size);
    if (vbuf == NULL)
    {
        EndSynchronousRequest();
        return VioGpuHostContextNotSubmitted;
    }

    BOOLEAN releaseBuffer = TRUE;
    BOOLEAN submitted = FALSE;
    BOOLEAN completed = SubmitSynchronousLocked(vbuf, &releaseBuffer, &submitted);
    PGPU_CTRL_HDR response = reinterpret_cast<PGPU_CTRL_HDR>(vbuf->resp_buf);
    VIOGPU_HOST_CONTEXT_RESULT result = VioGpuHostContextUnknown;
    if (!submitted)
    {
        result = VioGpuHostContextNotSubmitted;
    }
    else if (completed && vbuf->response_size == sizeof(GPU_CTRL_HDR) &&
             IsPlainControlResponse(response, VIRTIO_GPU_RESP_OK_NODATA))
    {
        result = VioGpuHostContextConfirmed;
    }
    else if (completed && vbuf->response_size == sizeof(GPU_CTRL_HDR) && IsPlainControlErrorResponse(response))
    {
        result = VioGpuHostContextRejected;
    }
    else if (completed)
    {
        PoisonSynchronousRequests();
    }
    if (releaseBuffer)
    {
        ReleaseBuffer(vbuf);
    }
    EndSynchronousRequest();
    return result;
}

PAGED_CODE_SEG_END

void CtrlQueue::DestroyResource(UINT res_id)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_RES_UNREF cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_UNREF)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    cmd->resource_id = res_id;

    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void CtrlQueue::DetachBacking(UINT res_id)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_RES_DETACH_BACKING cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_DETACH_BACKING)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    cmd->resource_id = res_id;

    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

PVOID CtrlQueue::AllocCmdResp(PGPU_VBUFFER *buf, int cmd_sz, PVOID resp_buf, int resp_sz)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER vbuf;
    vbuf = m_pBuf->GetBuf(cmd_sz, resp_sz, resp_buf);
    ASSERT(vbuf);
    *buf = vbuf;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return vbuf ? vbuf->buf : NULL;
}

PVOID CtrlQueue::AllocCmd(PGPU_VBUFFER *buf, int sz)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (buf == NULL || sz == 0)
    {
        return NULL;
    }

    PGPU_VBUFFER vbuf = m_pBuf->GetBuf(sz, sizeof(GPU_CTRL_HDR), NULL);
    ASSERT(vbuf);
    *buf = vbuf;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s  vbuf = %p\n", __FUNCTION__, vbuf));

    return vbuf ? vbuf->buf : NULL;
}

PGPU_VBUFFER CtrlQueue::PrepareNativeSubmit(UINT context_id, const void *command, UINT command_size)
{
    if (context_id == 0 || command == NULL || command_size == 0 || (command_size & (sizeof(ULONG) - 1)) != 0)
    {
        return NULL;
    }

    PGPU_VBUFFER vbuf = NULL;
    PGPU_CMD_SUBMIT_3D submit = static_cast<PGPU_CMD_SUBMIT_3D>(AllocCmd(&vbuf, sizeof(GPU_CMD_SUBMIT_3D)));
    if (submit == NULL)
    {
        return NULL;
    }

    PVOID payload = m_pBuf->AllocateMemory(command_size, sizeof(ULONGLONG));
    if (payload == NULL)
    {
        ReleaseBuffer(vbuf);
        return NULL;
    }

    RtlZeroMemory(submit, sizeof(*submit));
    submit->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    submit->hdr.ctx_id = context_id;
    submit->size = command_size;
    RtlCopyMemory(payload, command, command_size);
    InitializeListHead(&vbuf->native_submit_link);
    vbuf->data_buf = payload;
    vbuf->data_size = command_size;
    return vbuf;
}

BOOLEAN CtrlQueue::RefreshNativeSubmit(PGPU_VBUFFER buf, const void *command, UINT command_size)
{
    if (buf == NULL || command == NULL || command_size == 0 || buf->size != sizeof(GPU_CMD_SUBMIT_3D) ||
        buf->data_buf == NULL || buf->data_size != command_size)
    {
        return FALSE;
    }

    PGPU_CMD_SUBMIT_3D submit = reinterpret_cast<PGPU_CMD_SUBMIT_3D>(buf->buf);
    if (submit->hdr.type != VIRTIO_GPU_CMD_SUBMIT_3D || submit->size != command_size)
    {
        return FALSE;
    }

    RtlCopyMemory(buf->data_buf, command, command_size);
    return TRUE;
}

int CtrlQueue::QueueNativeSubmit(PGPU_VBUFFER buf, ULONGLONG fence_id)
{
    if (buf == NULL)
    {
        return -1;
    }

    /* DxgkDdiSubmitCommand is not allowed to return a transient queue-full
     * error: Dxgkrnl treats any error as a scheduler bugcheck.  Preserve
     * submission order and retain the already prepared nonpaged buffer until
     * a completed descriptor makes room. */
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeSubmitLock, &oldIrql);
    if (InterlockedCompareExchange(&m_NativeSubmitBacklogPoisoned, 0, 0) != 0)
    {
        KeReleaseSpinLock(&m_NativeSubmitLock, oldIrql);
        return -1;
    }
    /* Keep all buffer inspection and command-header publication inside the
     * same gate used by teardown.  Otherwise a D3/failed-start teardown could
     * reclaim this VioGpuBuf after the caller entered the function but before
     * it acquired m_NativeSubmitLock. */
    if (fence_id == 0 || fence_id > MAXUINT || buf->size != sizeof(GPU_CMD_SUBMIT_3D))
    {
        KeReleaseSpinLock(&m_NativeSubmitLock, oldIrql);
        return -1;
    }
    PGPU_CMD_SUBMIT_3D submit = reinterpret_cast<PGPU_CMD_SUBMIT_3D>(buf->buf);
    if (submit->hdr.type != VIRTIO_GPU_CMD_SUBMIT_3D || submit->hdr.ctx_id == 0 || submit->size == 0 ||
        submit->size != buf->data_size)
    {
        KeReleaseSpinLock(&m_NativeSubmitLock, oldIrql);
        return -1;
    }

    submit->hdr.flags = VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX;
    submit->hdr.fence_id = fence_id;
    submit->hdr.ring_idx = 1;

    if (!IsListEmpty(&m_NativeSubmitBacklog))
    {
        InsertTailList(&m_NativeSubmitBacklog, &buf->native_submit_link);
        KeReleaseSpinLock(&m_NativeSubmitLock, oldIrql);
        return 0;
    }

    int result = QueueBuffer(buf);
    if (result == -ENOSPC)
    {
        InsertTailList(&m_NativeSubmitBacklog, &buf->native_submit_link);
        result = 0;
    }
    else if (result < 0)
    {
        /* QueueBuffer has proved that this transport generation cannot
         * accept this prepared command.  Do not let a concurrent caller
         * continue feeding the same broken queue while the adapter reports
         * the scheduler fault and begins reset. */
        InterlockedExchange(&m_NativeSubmitBacklogPoisoned, 1);
    }
    KeReleaseSpinLock(&m_NativeSubmitLock, oldIrql);
    return result;
}

void CtrlQueue::DrainNativeSubmitBacklog(void)
{
    LIST_ENTRY quarantined;
    InitializeListHead(&quarantined);
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeSubmitLock, &oldIrql);
    if (InterlockedCompareExchange(&m_NativeSubmitBacklogPoisoned, 0, 0) != 0)
    {
        KeReleaseSpinLock(&m_NativeSubmitLock, oldIrql);
        return;
    }
    while (!IsListEmpty(&m_NativeSubmitBacklog))
    {
        PLIST_ENTRY entry = RemoveHeadList(&m_NativeSubmitBacklog);
        PGPU_VBUFFER buffer = CONTAINING_RECORD(entry, GPU_VBUFFER, native_submit_link);
        InitializeListHead(&buffer->native_submit_link);

        /* Do not touch buffer after a successful QueueBuffer call: the host
         * may retire it and run the completion callback on another CPU before
         * this routine returns. */
        int result = QueueBuffer(buffer);
        if (result == -ENOSPC)
        {
            InsertHeadList(&m_NativeSubmitBacklog, &buffer->native_submit_link);
            break;
        }
        if (result < 0)
        {
            /* A non-ENOSPC result is a permanent enqueue failure for this
             * transport generation.  Poison the software queue and move the
             * failed entry plus every remaining entry to a local quarantine
             * list.  They remain VioGpuBuf-owned until callbacks have
             * released their higher-level submission records below. */
            InterlockedExchange(&m_NativeSubmitBacklogPoisoned, 1);
            InsertTailList(&quarantined, &buffer->native_submit_link);
            while (!IsListEmpty(&m_NativeSubmitBacklog))
            {
                PLIST_ENTRY remaining = RemoveHeadList(&m_NativeSubmitBacklog);
                InsertTailList(&quarantined, remaining);
            }
            break;
        }
    }
    KeReleaseSpinLock(&m_NativeSubmitLock, oldIrql);

    while (!IsListEmpty(&quarantined))
    {
        PLIST_ENTRY entry = RemoveHeadList(&quarantined);
        PGPU_VBUFFER failedBuffer = CONTAINING_RECORD(entry, GPU_VBUFFER, native_submit_link);
        InitializeListHead(&failedBuffer->native_submit_link);
        VIOGPU_VBUFFER_TERMINAL_CLAIM claim = VioGpuClaimVbufferTerminalCallbacks(failedBuffer);
        if (claim != VioGpuVbufferTerminalClaimLost)
        {
            void (*errorCallback)(void *) = failedBuffer->queue_error_cb;
            void *errorContext = failedBuffer->queue_error_ctx;
            VioGpuDetachVbufferTerminalCallbacks(failedBuffer);
            if (errorCallback != NULL)
            {
                errorCallback(errorContext);
            }
        }
        if (claim != VioGpuVbufferTerminalClaimLost)
        {
            ReleaseBuffer(failedBuffer);
        }
    }
}

void CtrlQueue::PoisonNativeSubmitBacklog(void)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeSubmitLock, &oldIrql);
    InterlockedExchange(&m_NativeSubmitBacklogPoisoned, 1);
    KeReleaseSpinLock(&m_NativeSubmitLock, oldIrql);
}

void CtrlQueue::DetachNativeSubmitBacklog(void)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeSubmitLock, &oldIrql);
    InterlockedExchange(&m_NativeSubmitBacklogPoisoned, 1);
    while (!IsListEmpty(&m_NativeSubmitBacklog))
    {
        PLIST_ENTRY entry = RemoveHeadList(&m_NativeSubmitBacklog);
        PGPU_VBUFFER buffer = CONTAINING_RECORD(entry, GPU_VBUFFER, native_submit_link);
        InitializeListHead(&buffer->native_submit_link);
    }
    KeReleaseSpinLock(&m_NativeSubmitLock, oldIrql);
}

BOOLEAN CtrlQueue::ResetNativeSubmitBacklog(void)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_NativeSubmitLock, &oldIrql);
    BOOLEAN empty = IsListEmpty(&m_NativeSubmitBacklog);
    if (empty)
    {
        InterlockedExchange(&m_NativeSubmitBacklogPoisoned, 0);
    }
    KeReleaseSpinLock(&m_NativeSubmitLock, oldIrql);
    return empty;
}

void CtrlQueue::SetScanout(UINT scan_id, UINT res_id, UINT width, UINT height, UINT x, UINT y)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_SET_SCANOUT cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_SET_SCANOUT)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd->resource_id = res_id;
    cmd->scanout_id = scan_id;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->r.x = x;
    cmd->r.y = y;

    // FIXME if
    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

#define SGLIST_SIZE 256
static const int VIOGPU_QUEUE_ERROR = -1;

int CtrlQueue::QueueBuffer(PGPU_VBUFFER buf)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VirtIOBufferDescriptor sg[SGLIST_SIZE];
    UINT sgleft = SGLIST_SIZE;
    UINT outcnt = 0, incnt = 0;
    int ret = 0;
    KIRQL SavedIrql;

    if (buf->size > PAGE_SIZE)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--> %s size is too big %d\n", __FUNCTION__, buf->size));
        return VIOGPU_QUEUE_ERROR;
    }

    if (!BuildSGElement(&sg[outcnt + incnt], (PVOID)buf->buf, buf->size))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--> %s invalid command DMA address %p\n", __FUNCTION__, buf->buf));
        return VIOGPU_QUEUE_ERROR;
    }
    else
    {
        outcnt++;
        sgleft--;
    }

    if (buf->data_size)
    {
        ULONG data_size = buf->data_size;
        PVOID data_buf = (PVOID)buf->data_buf;
        while (data_size)
        {
            if (BuildSGElement(&sg[outcnt + incnt], data_buf, data_size))
            {
                ULONG segmentSize = sg[outcnt + incnt].length;
                data_buf = (PVOID)((PUCHAR)data_buf + segmentSize);
                data_size -= segmentSize;
                outcnt++;
                sgleft--;
                if (sgleft == 0)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("<--> %s no more sgelenamt spots left %d\n", __FUNCTION__, outcnt));
                    return VIOGPU_QUEUE_ERROR;
                }
            }
            else
            {
                DbgPrint(TRACE_LEVEL_ERROR, ("<--> %s invalid data DMA address %p\n", __FUNCTION__, data_buf));
                return VIOGPU_QUEUE_ERROR;
            }
        }
    }

    if (buf->resp_size > PAGE_SIZE)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--> %s resp_size is too big %d\n", __FUNCTION__, buf->resp_size));
        return VIOGPU_QUEUE_ERROR;
    }

    if (buf->resp_size && (sgleft > 0))
    {
        if (!BuildSGElement(&sg[outcnt + incnt], (PVOID)buf->resp_buf, buf->resp_size))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("<--> %s invalid response DMA address %p\n", __FUNCTION__, buf->resp_buf));
            return VIOGPU_QUEUE_ERROR;
        }
        else
        {
            incnt++;
            sgleft--;
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s sgleft %d\n", __FUNCTION__, sgleft));

    buf->response_size = 0;
    Lock(&SavedIrql);
    ret = AddBuf(&sg[0], outcnt, incnt, buf, NULL, 0);
    if (ret >= 0)
    {
        Kick();
    }
    Unlock(SavedIrql);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s ret = %d\n", __FUNCTION__, ret));

    return ret;
}

PGPU_VBUFFER CtrlQueue::DequeueBuffer(_Out_ UINT *len)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER buf = NULL;
    KIRQL SavedIrql;
    Lock(&SavedIrql);
    buf = (PGPU_VBUFFER)GetBuf(len);
    Unlock(SavedIrql);
    if (buf == NULL)
    {
        *len = 0;
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return buf;
}

void VioGpuQueue::ReleaseBuffer(PGPU_VBUFFER buf)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    m_pBuf->FreeBuf(buf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuBuf::Init(_In_ UINT cnt)
{
    KIRQL OldIrql;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (cnt == 0)
    {
        return FALSE;
    }
    KeAcquireSpinLock(&m_SpinLock, &OldIrql);
    m_uCountMin = cnt;
    UINT allocateCount = m_uCount < cnt ? cnt - m_uCount : 0;
    KeReleaseSpinLock(&m_SpinLock, OldIrql);

    for (UINT i = 0; i < allocateCount; ++i)
    {
        PGPU_VBUFFER pvbuf = reinterpret_cast<PGPU_VBUFFER>(AllocateMemory(VBUFFER_SIZE));
        if (pvbuf)
        {
            RtlZeroMemory(pvbuf, VBUFFER_SIZE);
            KeAcquireSpinLock(&m_SpinLock, &OldIrql);
            InsertTailList(&m_FreeBufs, &pvbuf->list_entry);
            ++m_uCount;
            KeReleaseSpinLock(&m_SpinLock, OldIrql);
        }
    }
    ASSERT(m_uCount >= cnt);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    if (m_uCount < cnt)
    {
        return FALSE;
    }
    return TRUE;
}

BOOLEAN VioGpuBuf::Close(void)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    LIST_ENTRY buffers;
    InitializeListHead(&buffers);
    BOOLEAN drained = TRUE;

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_SpinLock, &oldIrql);
    while (!IsListEmpty(&m_InUseBufs))
    {
        LIST_ENTRY *entry = RemoveHeadList(&m_InUseBufs);
        PGPU_VBUFFER buffer = CONTAINING_RECORD(entry, GPU_VBUFFER, list_entry);
        buffer->response_size = 0;
        InsertTailList(&buffers, entry);
    }
    while (!IsListEmpty(&m_FreeBufs))
    {
        InsertTailList(&buffers, RemoveHeadList(&m_FreeBufs));
    }
    m_uCount = 0;
    m_uCountMin = 0;
    KeReleaseSpinLock(&m_SpinLock, oldIrql);

    // Callbacks and frees run after the lists are detached from the spin lock.
    while (!IsListEmpty(&buffers))
    {
        LIST_ENTRY *entry = RemoveHeadList(&buffers);
        PGPU_VBUFFER buffer = CONTAINING_RECORD(entry, GPU_VBUFFER, list_entry);
        VIOGPU_VBUFFER_TERMINAL_CLAIM claim = VioGpuClaimVbufferTerminalCallbacks(buffer);
        if (claim != VioGpuVbufferTerminalClaimLost)
        {
            void (*cancelCallback)(void *) = buffer->cancel_cb;
            void *cancelContext = buffer->cancel_ctx;
            VioGpuDetachVbufferTerminalCallbacks(buffer);
            if (cancelCallback != NULL)
            {
                cancelCallback(cancelContext);
            }
            VioGpuCompleteVbufferTerminalCallbacks(buffer);
        }
        else if (!VioGpuWaitForVbufferTerminalCallbacks(buffer))
        {
            KeAcquireSpinLock(&m_SpinLock, &oldIrql);
            InsertTailList(&m_InUseBufs, &buffer->list_entry);
            ++m_uCount;
            KeReleaseSpinLock(&m_SpinLock, oldIrql);
            drained = FALSE;
            continue;
        }
        if (buffer->resp_buf != NULL && buffer->resp_size > MAX_INLINE_RESP_SIZE)
        {
            FreeMemory(buffer->resp_buf);
        }
        if (buffer->data_buf != NULL && buffer->data_size != 0)
        {
            FreeMemory(buffer->data_buf);
        }
        FreeMemory(buffer);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return drained;
}

PGPU_VBUFFER VioGpuBuf::GetBuf(_In_ int size, _In_ int resp_size, _In_opt_ void *resp_buf)
{

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER pbuf = NULL;
    PLIST_ENTRY pListItem = NULL;
    KIRQL SavedIrql = KeGetCurrentIrql();

    if (SavedIrql < DISPATCH_LEVEL)
    {
        KeAcquireSpinLock(&m_SpinLock, &SavedIrql);
    }
    else if (SavedIrql == DISPATCH_LEVEL)
    {
        KeAcquireSpinLockAtDpcLevel(&m_SpinLock);
    }
    else
    {
        // This is possible situation in case of bugcheck.
        // DxgkDdiSystemDisplayEnable can be called at any IRQL.
        // We need to allocate buffer for several command during this proccess.
        // VioGpuDbgBreak();
    }

    if (IsListEmpty(&m_FreeBufs))
    {
        pbuf = reinterpret_cast<PGPU_VBUFFER>(AllocateMemory(VBUFFER_SIZE));
        if (pbuf != NULL)
        {
            ++m_uCount;
        }
    }
    else
    {
        pListItem = RemoveHeadList(&m_FreeBufs);
        pbuf = CONTAINING_RECORD(pListItem, GPU_VBUFFER, list_entry);
    }

    if (pbuf == NULL)
    {
        if (SavedIrql < DISPATCH_LEVEL)
        {
            KeReleaseSpinLock(&m_SpinLock, SavedIrql);
        }
        else if (SavedIrql == DISPATCH_LEVEL)
        {
            KeReleaseSpinLockFromDpcLevel(&m_SpinLock);
        }
        return NULL;
    }
    memset(pbuf, 0, VBUFFER_SIZE);
    KeInitializeEvent(&pbuf->completion_event, NotificationEvent, FALSE);
    KeInitializeEvent(&pbuf->terminal_callback_event, NotificationEvent, TRUE);
    ASSERT(size <= MAX_INLINE_CMD_SIZE);

    pbuf->buf = (char *)((ULONG_PTR)pbuf + sizeof(*pbuf));
    pbuf->size = size;
    pbuf->auto_release = true;

    pbuf->resp_size = resp_size;
    if (resp_size <= MAX_INLINE_RESP_SIZE)
    {
        pbuf->resp_buf = (char *)((ULONG_PTR)pbuf->buf + size);
    }
    else
    {
        pbuf->resp_buf = (char *)resp_buf;
    }
    ASSERT(pbuf->resp_buf);
    InsertTailList(&m_InUseBufs, &pbuf->list_entry);

    if (SavedIrql < DISPATCH_LEVEL)
    {
        KeReleaseSpinLock(&m_SpinLock, SavedIrql);
    }
    else if (SavedIrql == DISPATCH_LEVEL)
    {
        KeReleaseSpinLockFromDpcLevel(&m_SpinLock);
    }
    else
    {
        // This is possible situation in case of bugcheck.
        // DxgkDdiSystemDisplayEnable can be called at any IRQL.
        // We need to allocate buffer for several command during this proccess.
        // VioGpuDbgBreak();
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s buf = %p\n", __FUNCTION__, pbuf));

    return pbuf;
}

void VioGpuBuf::FreeBuf(_In_ PGPU_VBUFFER pbuf)
{
    if (pbuf == NULL)
    {
        return;
    }

    KIRQL OldIrql;
    PVOID response = NULL;
    PVOID data = NULL;
    BOOLEAN found = FALSE;
    BOOLEAN freeBuffer = FALSE;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s buf = %p\n", __FUNCTION__, pbuf));
    KeAcquireSpinLock(&m_SpinLock, &OldIrql);

    for (PLIST_ENTRY entry = m_InUseBufs.Flink; entry != &m_InUseBufs; entry = entry->Flink)
    {
        PGPU_VBUFFER buffer = CONTAINING_RECORD(entry, GPU_VBUFFER, list_entry);
        if (buffer == pbuf)
        {
            RemoveEntryList(entry);
            found = TRUE;
            break;
        }
    }

    if (found)
    {
        VioGpuDetachVbufferTerminalCallbacks(pbuf);
        if (pbuf->resp_buf != NULL && pbuf->resp_size > MAX_INLINE_RESP_SIZE)
        {
            response = pbuf->resp_buf;
            pbuf->resp_buf = NULL;
            pbuf->resp_size = 0;
        }
        if (pbuf->data_buf != NULL && pbuf->data_size != 0)
        {
            data = pbuf->data_buf;
            pbuf->data_buf = NULL;
            pbuf->data_size = 0;
        }
        freeBuffer = m_uCount > m_uCountMin;
        if (freeBuffer)
        {
            --m_uCount;
        }
    }
    KeReleaseSpinLock(&m_SpinLock, OldIrql);

    if (!found)
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("<--- %s ignored unowned buf = %p\n", __FUNCTION__, pbuf));
        VioGpuCompleteVbufferTerminalCallbacks(pbuf);
        return;
    }

    FreeMemory(response);
    FreeMemory(data);

    if (freeBuffer)
    {
        VioGpuCompleteVbufferTerminalCallbacks(pbuf);
        FreeMemory(pbuf);
    }
    else
    {
        VioGpuCompleteVbufferTerminalCallbacks(pbuf);
        KeAcquireSpinLock(&m_SpinLock, &OldIrql);
        InsertTailList(&m_FreeBufs, &pbuf->list_entry);
        KeReleaseSpinLock(&m_SpinLock, OldIrql);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void VioGpuBuf::ReclaimBuffers(void)
{
    const UINT keepCount = m_uCountMin;
    LIST_ENTRY reclaimed;
    LIST_ENTRY surplus;
    InitializeListHead(&reclaimed);
    InitializeListHead(&surplus);

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_SpinLock, &oldIrql);
    while (!IsListEmpty(&m_InUseBufs))
    {
        LIST_ENTRY *entry = RemoveHeadList(&m_InUseBufs);
        PGPU_VBUFFER buffer = CONTAINING_RECORD(entry, GPU_VBUFFER, list_entry);
        buffer->response_size = 0;
        InsertTailList(&reclaimed, entry);
    }
    while (m_uCount > keepCount && !IsListEmpty(&m_FreeBufs))
    {
        LIST_ENTRY *entry = RemoveHeadList(&m_FreeBufs);
        InsertTailList(&surplus, entry);
        --m_uCount;
    }
    KeReleaseSpinLock(&m_SpinLock, oldIrql);

    while (!IsListEmpty(&reclaimed))
    {
        LIST_ENTRY *entry = RemoveHeadList(&reclaimed);
        PGPU_VBUFFER buffer = CONTAINING_RECORD(entry, GPU_VBUFFER, list_entry);
        VIOGPU_VBUFFER_TERMINAL_CLAIM claim = VioGpuClaimVbufferTerminalCallbacks(buffer);
        if (claim != VioGpuVbufferTerminalClaimLost)
        {
            void (*cancelCallback)(void *) = buffer->cancel_cb;
            void *cancelContext = buffer->cancel_ctx;
            VioGpuDetachVbufferTerminalCallbacks(buffer);
            if (cancelCallback != NULL)
            {
                cancelCallback(cancelContext);
            }
            VioGpuCompleteVbufferTerminalCallbacks(buffer);
        }
        else if (!VioGpuWaitForVbufferTerminalCallbacks(buffer))
        {
            KeAcquireSpinLock(&m_SpinLock, &oldIrql);
            InsertTailList(&m_InUseBufs, &buffer->list_entry);
            KeReleaseSpinLock(&m_SpinLock, oldIrql);
            continue;
        }
        if (buffer->resp_buf != NULL && buffer->resp_size > MAX_INLINE_RESP_SIZE)
        {
            FreeMemory(buffer->resp_buf);
            buffer->resp_buf = NULL;
            buffer->resp_size = 0;
        }
        if (buffer->data_buf != NULL && buffer->data_size != 0)
        {
            FreeMemory(buffer->data_buf);
            buffer->data_buf = NULL;
            buffer->data_size = 0;
        }

        BOOLEAN freeBuffer;
        KeAcquireSpinLock(&m_SpinLock, &oldIrql);
        freeBuffer = m_uCount > keepCount;
        if (freeBuffer)
        {
            --m_uCount;
        }
        else
        {
            InsertTailList(&m_FreeBufs, &buffer->list_entry);
        }
        KeReleaseSpinLock(&m_SpinLock, oldIrql);
        if (freeBuffer)
        {
            FreeMemory(buffer);
        }
    }

    while (!IsListEmpty(&surplus))
    {
        LIST_ENTRY *entry = RemoveHeadList(&surplus);
        PGPU_VBUFFER buffer = CONTAINING_RECORD(entry, GPU_VBUFFER, list_entry);
        FreeMemory(buffer);
    }
}

PAGED_CODE_SEG_BEGIN
VioGpuBuf::VioGpuBuf()
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    InitializeListHead(&m_FreeBufs);
    InitializeListHead(&m_InUseBufs);
    KeInitializeSpinLock(&m_SpinLock);
    m_uCount = 0;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuBuf::~VioGpuBuf()
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s 0x%p\n", __FUNCTION__, this));

    Close();

    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
}

PVOID VioGpuBuf::AllocateMemory(SIZE_T size, SIZE_T alignment)
{
    UNREFERENCED_PARAMETER(alignment);
    PVOID address = ExAllocatePoolUninitialized(NonPagedPoolNx, size, VIOGPUTAG);
    if (address != NULL)
    {
        RtlZeroMemory(address, size);
    }
    return address;
}

void VioGpuBuf::FreeMemory(PVOID address)
{
    if (address == NULL)
    {
        return;
    }
    ExFreePoolWithTag(address, VIOGPUTAG);
}

VioGpuMemSegment::VioGpuMemSegment(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    m_pSGList = NULL;
    m_pVAddr = NULL;
    m_pMdl = NULL;
    m_bSystemMemory = FALSE;
    m_bMapped = FALSE;
    m_Size = 0;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuMemSegment::~VioGpuMemSegment(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    Close();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuMemSegment::Init(_In_ UINT size, _In_opt_ PPHYSICAL_ADDRESS pPAddr)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    ASSERT(size);
    if (size == 0 || size > MAXUINT - (PAGE_SIZE - 1))
    {
        return FALSE;
    }
    PVOID buf = NULL;
    UINT pages = BYTES_TO_PAGES(size);
    SIZE_T sglsize = FIELD_OFFSET(SCATTER_GATHER_LIST, Elements) +
                     (sizeof(SCATTER_GATHER_ELEMENT) * static_cast<SIZE_T>(pages));
    size = pages * PAGE_SIZE;
    if ((pPAddr == NULL) || pPAddr->QuadPart == 0LL)
    {
        m_pVAddr = new (NonPagedPoolNx) BYTE[size];

        if (!m_pVAddr)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s insufficient resources to allocate %x bytes\n", __FUNCTION__, size));
            return FALSE;
        }
        RtlZeroMemory(m_pVAddr, size);
        m_bSystemMemory = TRUE;
    }
    else
    {
        NTSTATUS Status = MapFrameBuffer(*pPAddr, size, &m_pVAddr);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s MapFrameBuffer failed with Status: 0x%X\n", __FUNCTION__, Status));
            return FALSE;
        }
        m_bMapped = TRUE;
    }
    // Close() needs the complete mapping extent even when a later MDL or SG
    // construction step fails.
    m_Size = size;

    {
        m_pMdl = IoAllocateMdl(m_pVAddr, size, FALSE, FALSE, NULL);
        if (!m_pMdl)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s insufficient resources to allocate MDLs\n", __FUNCTION__));
            Close();
            return FALSE;
        }
        if (m_bSystemMemory == TRUE)
        {
            __try
            {
                MmProbeAndLockPages(m_pMdl, KernelMode, IoWriteAccess);
            }
#pragma prefast(suppress : __WARNING_EXCEPTIONEXECUTEHANDLER, "try/except is only able to protect against user-mode errors and these are the only errors we try to catch here");
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                DbgPrint(TRACE_LEVEL_FATAL,
                         ("%s Failed to lock pages with error %x\n", __FUNCTION__, GetExceptionCode()));
                IoFreeMdl(m_pMdl);
                m_pMdl = NULL;
                Close();
                return FALSE;
            }
        }
    }
    m_pSGList = reinterpret_cast<PSCATTER_GATHER_LIST>(new (NonPagedPoolNx) BYTE[sglsize]);
    if (m_pSGList == NULL)
    {
        Close();
        return FALSE;
    }
    m_pSGList->NumberOfElements = 0;
    m_pSGList->Reserved = 0;
    //       m_pSAddr = reinterpret_cast<BYTE*>
    //    (MmGetSystemAddressForMdlSafe(m_pMdl, NormalPagePriority | MdlMappingNoExecute));

    RtlZeroMemory(m_pSGList, sglsize);
    buf = PAGE_ALIGN(m_pVAddr);

    for (UINT i = 0; i < pages; ++i)
    {
        PHYSICAL_ADDRESS pa = {0};
        ASSERT(MmIsAddressValid(buf));
        pa = MmGetPhysicalAddress(buf);
        if (pa.QuadPart == 0LL)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s Invalid PA buf = %p element %d\n", __FUNCTION__, buf, i));
            Close();
            return FALSE;
        }
        m_pSGList->Elements[i].Address = pa;
        m_pSGList->Elements[i].Length = PAGE_SIZE;
        buf = (PVOID)((LONG_PTR)(buf) + PAGE_SIZE);
        m_pSGList->NumberOfElements++;
    }
    if (m_pSGList->NumberOfElements != pages)
    {
        Close();
        return FALSE;
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return TRUE;
}

PHYSICAL_ADDRESS VioGpuMemSegment::GetPhysicalAddress(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PHYSICAL_ADDRESS pa = {0};
    if (m_pVAddr && MmIsAddressValid(m_pVAddr))
    {
        pa = MmGetPhysicalAddress(m_pVAddr);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return pa;
}

void VioGpuMemSegment::Close(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (m_pMdl)
    {
        if (m_bSystemMemory)
        {
            MmUnlockPages(m_pMdl);
        }
        IoFreeMdl(m_pMdl);
        m_pMdl = NULL;
    }

    if (m_pVAddr != NULL && m_bSystemMemory)
    {
        delete[] reinterpret_cast<PBYTE>(m_pVAddr);
    }
    else if (m_pVAddr != NULL && m_bMapped)
    {
        (void)UnmapFrameBuffer(m_pVAddr, (ULONG)m_Size);
    }
    m_pVAddr = NULL;

    delete[] reinterpret_cast<PBYTE>(m_pSGList);
    m_pSGList = NULL;
    m_bSystemMemory = FALSE;
    m_bMapped = FALSE;
    m_Size = 0;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuObj::VioGpuObj(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    m_uiHwRes = 0;
    m_pSegment = NULL;
    m_Size = 0;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuObj::~VioGpuObj(void)
{
    // Driver can destroy object in case of a bugcheck.
    // DxgkDdiSystemDisplayEnable can be called at any IRQL, so it must
    // be in nonpageable memory. DxgkDdiSystemDisplayEnable must not
    // call any code that is in pageable memory and must not manipulate
    // any data that is in pageable memory.
    // PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuObj::Init(_In_ UINT size, VioGpuMemSegment *pSegment)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s requested size = %d\n", __FUNCTION__, size));

    ASSERT(size);
    ASSERT(pSegment);
    UINT pages = BYTES_TO_PAGES(size);
    size = pages * PAGE_SIZE;
    if (size > pSegment->GetSize())
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("<--- %s segment size too small = %Iu (%u)\n", __FUNCTION__, pSegment->GetSize(), size));
        return FALSE;
    }
    m_pSegment = pSegment;
    m_Size = size;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s size = %Iu\n", __FUNCTION__, m_Size));
    return TRUE;
}

PVOID CrsrQueue::AllocCursor(PGPU_VBUFFER *buf)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER vbuf;
    vbuf = m_pBuf->GetBuf(sizeof(GPU_UPDATE_CURSOR), 0, NULL);
    ASSERT(vbuf);
    *buf = vbuf;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s  vbuf = %p\n", __FUNCTION__, vbuf));

    return vbuf ? vbuf->buf : NULL;
}

PAGED_CODE_SEG_END

UINT CrsrQueue::QueueCursor(PGPU_VBUFFER buf)
{
    //    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    UINT res = 0;
    KIRQL SavedIrql;

    VirtIOBufferDescriptor sg[1];
    int outcnt = 0;
    int ret = 0;

    ASSERT(buf->size <= PAGE_SIZE);
    if (BuildSGElement(&sg[outcnt], (PVOID)buf->buf, buf->size))
    {
        outcnt++;
    }
    else
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--> %s invalid cursor DMA address %p\n", __FUNCTION__, buf->buf));
        return 0;
    }
    Lock(&SavedIrql);
    ret = AddBuf(&sg[0], outcnt, 0, buf, NULL, 0);
    if (ret >= 0)
    {
        Kick();
    }
    Unlock(SavedIrql);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s vbuf = %p outcnt = %d, ret = %d\n", __FUNCTION__, buf, outcnt, ret));
    return res;
}

PGPU_VBUFFER CrsrQueue::DequeueCursor(_Out_ UINT *len)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER buf = NULL;
    KIRQL SavedIrql;
    Lock(&SavedIrql);
    buf = (PGPU_VBUFFER)GetBuf(len);
    Unlock(SavedIrql);
    if (buf == NULL)
    {
        *len = 0;
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s buf %p len = %u\n", __FUNCTION__, buf, *len));
    return buf;
}
