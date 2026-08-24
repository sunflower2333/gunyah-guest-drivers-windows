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

#pragma once
#include "viogpu.h"
#include "viogpu_pci.h"

#pragma pack(1)
typedef struct virtio_gpu_config
{
    u32 events_read;
    u32 events_clear;
    u32 num_scanouts;
    u32 num_capsets;
} GPU_CONFIG, *PGPU_CONFIG;
#pragma pack()

// #pragma pack(1)
typedef struct virtio_gpu_vbuffer
{
    char *buf;
    int size;

    void *data_buf;
    u32 data_size;

    char *resp_buf;
    int resp_size;
    LIST_ENTRY list_entry;

    void (*complete_cb)(void *ctx);
    void *complete_ctx;

    /* Invoked when an in-flight buffer is reclaimed by queue reset/close.
     * The queue owns the buffer in this path, so the callback must release
     * only the higher-level command ownership and must not call FreeBuf(). */
    void (*cancel_cb)(void *ctx);
    void *cancel_ctx;

    /* Invoked if a prepared native submit encounters a permanent enqueue
     * failure while being drained from the software backlog. */
    void (*queue_error_cb)(void *ctx);
    void *queue_error_ctx;

    /* Completion, reset cancellation, and permanent backlog failure all race
     * for one higher-level submission owner.  Only the path which changes
     * Armed to Claimed may detach and invoke a terminal callback. */
    volatile LONG terminal_callback_state;
    KEVENT terminal_callback_event;

    /* A native submit may wait in the software backlog while the control
     * virtqueue has no free descriptor.  This link is owned by CtrlQueue,
     * not by VioGpuBuf's in-use/free lists. */
    LIST_ENTRY native_submit_link;

    bool auto_release;
    KEVENT completion_event;
    UINT response_size;
    LONG64 synchronous_epoch_state;
} GPU_VBUFFER, *PGPU_VBUFFER;
// #pragma pack()

enum VIOGPU_VBUFFER_TERMINAL_STATE : LONG
{
    VioGpuVbufferTerminalUnarmed = 0,
    VioGpuVbufferTerminalArmed,
    VioGpuVbufferTerminalClaimed,
    VioGpuVbufferTerminalCompleted,
};

enum VIOGPU_VBUFFER_TERMINAL_CLAIM : LONG
{
    VioGpuVbufferTerminalClaimUnarmed = 0,
    VioGpuVbufferTerminalClaimWon,
    VioGpuVbufferTerminalClaimLost,
};

BOOLEAN VioGpuArmVbufferTerminalCallbacks(_Inout_ PGPU_VBUFFER buffer);
VIOGPU_VBUFFER_TERMINAL_CLAIM VioGpuClaimVbufferTerminalCallbacks(_Inout_ PGPU_VBUFFER buffer);
VOID VioGpuDetachVbufferTerminalCallbacks(_Inout_ PGPU_VBUFFER buffer);
VOID VioGpuCompleteVbufferTerminalCallbacks(_Inout_ PGPU_VBUFFER buffer);
BOOLEAN VioGpuWaitForVbufferTerminalCallbacks(_Inout_ PGPU_VBUFFER buffer);

enum VIOGPU_SYNCHRONOUS_STATE : LONG
{
    VioGpuSynchronousOffline = 0,
    VioGpuSynchronousEnabled,
    VioGpuSynchronousQuiescing,
    VioGpuSynchronousPoisoned,
};

enum VIOGPU_HOST_CONTEXT_RESULT : LONG
{
    VioGpuHostContextNotSubmitted = 0,
    VioGpuHostContextConfirmed,
    VioGpuHostContextRejected,
    VioGpuHostContextUnknown,
};

/* Captures the control-queue response before the reusable buffer is released.
 * This is diagnostic state only; it does not change the native-context wire
 * contract or the result classification. */
typedef struct viogpu_host_context_response_diagnostic
{
    UINT ResponseSize;
    UINT Type;
    UINT Flags;
    ULONGLONG FenceId;
    UINT ContextId;
    UCHAR RingIndex;
    UCHAR Padding[3];
    BOOLEAN Submitted;
    BOOLEAN Completed;
} VIOGPU_HOST_CONTEXT_RESPONSE_DIAGNOSTIC, *PVIOGPU_HOST_CONTEXT_RESPONSE_DIAGNOSTIC;

enum VIOGPU_2D_RESOURCE_STATE : LONG
{
    VioGpu2DResourceNone = 0,
    VioGpu2DResourceCreated,
    VioGpu2DResourceBackingAttached,
    VioGpu2DResourceUnknown,
};

#define MAX_INLINE_CMD_SIZE             96
#define MAX_INLINE_RESP_SIZE            24
#define VBUFFER_SIZE                    (sizeof(GPU_VBUFFER) + MAX_INLINE_CMD_SIZE + MAX_INLINE_RESP_SIZE)
#define VIOGPU_NATIVE_CONTROL_BLOB_SIZE 0x4000U
#define VIOGPU_MAX_BACKING_ENTRIES      16384U

class VioGpuBuf
{
  public:
    VioGpuBuf();
    ~VioGpuBuf();
    PGPU_VBUFFER GetBuf(_In_ int size, _In_ int resp_size, _In_opt_ void *resp_buf);
    void FreeBuf(_In_ PGPU_VBUFFER pbuf);
    BOOLEAN Init(_In_ UINT cnt);
    void ReclaimBuffers(void);
    BOOLEAN Close(void);
    BOOLEAN HasAllocationOwner(void) const
    {
        return m_uCount != 0;
    }
    PVOID AllocateMemory(SIZE_T size, SIZE_T alignment = PAGE_SIZE);
    void FreeMemory(PVOID address);

  private:
    LIST_ENTRY m_FreeBufs;
    LIST_ENTRY m_InUseBufs;
    KSPIN_LOCK m_SpinLock;
    UINT m_uCount;
    UINT m_uCountMin = 0;
};

class VioGpuMemSegment
{
  public:
    VioGpuMemSegment(void);
    ~VioGpuMemSegment(void);
    SIZE_T GetSize(void)
    {
        return m_Size;
    }
    PVOID GetVirtualAddress(void)
    {
        return m_pVAddr;
    }
    PHYSICAL_ADDRESS GetPhysicalAddress(void);
    PSCATTER_GATHER_LIST GetSGList(void)
    {
        return m_pSGList;
    }
    BOOLEAN Init(_In_ UINT size, _In_opt_ PPHYSICAL_ADDRESS pPAddr);
    BOOLEAN IsSystemMemory(void)
    {
        return m_bSystemMemory;
    }
    void Close(void);

  private:
    BOOLEAN m_bSystemMemory;
    BOOLEAN m_bMapped;
    PSCATTER_GATHER_LIST m_pSGList;
    PVOID m_pVAddr;
    PMDL m_pMdl;
    SIZE_T m_Size;
};

class VioGpuObj
{
  public:
    VioGpuObj(void);
    ~VioGpuObj(void);
    void SetId(_In_ UINT id)
    {
        m_uiHwRes = id;
    }
    UINT GetId(void)
    {
        return m_uiHwRes;
    }
    BOOLEAN Init(_In_ UINT size, VioGpuMemSegment *pSegment);
    SIZE_T GetSize(void)
    {
        return m_Size;
    }
    PSCATTER_GATHER_LIST GetSGList(void)
    {
        return m_pSegment ? m_pSegment->GetSGList() : NULL;
    }
    PHYSICAL_ADDRESS GetPhysicalAddress(void)
    {
        PHYSICAL_ADDRESS pa = {0};
        return m_pSegment ? m_pSegment->GetPhysicalAddress() : pa;
    }
    PVOID GetVirtualAddress(void)
    {
        return m_pSegment ? m_pSegment->GetVirtualAddress() : NULL;
    }

  private:
    UINT m_uiHwRes;
    SIZE_T m_Size;
    VioGpuMemSegment *m_pSegment;
};

class VioGpuQueue
{
  public:
    VioGpuQueue();
    ~VioGpuQueue();
    BOOLEAN Init(_In_ VirtIODevice *pVIODevice, _In_ struct virtqueue *pVirtQueue, _In_ UINT index);
    void Close(void);
    int AddBuf(_In_ struct VirtIOBufferDescriptor sg[],
               _In_ UINT out_num,
               _In_ UINT in_num,
               _In_ void *data,
               _In_opt_ void *va_indirect,
               _In_ ULONGLONG phys_indirect)
    {
        return m_pVirtQueue ? virtqueue_add_buf(m_pVirtQueue, sg, out_num, in_num, data, va_indirect, phys_indirect)
                            : -1;
    }
    void *GetBuf(_Out_ UINT *len)
    {
        if (m_pVirtQueue)
        {
            return virtqueue_get_buf(m_pVirtQueue, len);
        }
        *len = 0;
        return NULL;
    }
    void Kick()
    {
        if (m_pVirtQueue)
        {
            virtqueue_kick_always(m_pVirtQueue);
        }
    }
    bool EnableInterrupt(void)
    {
        return m_pVirtQueue ? virtqueue_enable_cb(m_pVirtQueue) : false;
    }
    VOID DisableInterrupt(void)
    {
        if (m_pVirtQueue)
        {
            virtqueue_disable_cb(m_pVirtQueue);
        }
    }
    UINT QueryAllocation();
    void SetGpuBuf(_In_ VioGpuBuf *pbuf)
    {
        m_pBuf = pbuf;
    }
    void ReleaseBuffer(PGPU_VBUFFER buf);

  protected:
    _IRQL_requires_max_(DISPATCH_LEVEL) _IRQL_saves_global_(OldIrql,
                                                            Irql) _IRQL_raises_(DISPATCH_LEVEL) void Lock(KIRQL *Irql);
    _IRQL_requires_(DISPATCH_LEVEL) _IRQL_restores_global_(OldIrql, Irql) void Unlock(KIRQL Irql);

  private:
    struct virtqueue *m_pVirtQueue;
    VirtIODevice *m_pVIODevice;
    UINT m_Index;
    KSPIN_LOCK m_SpinLock;

  protected:
    VioGpuBuf *m_pBuf;
};

class CtrlQueue : public VioGpuQueue
{
  public:
    CtrlQueue() : VioGpuQueue()
    {
        m_FenceIdr = 0;
        KeInitializeMutex(&m_SynchronousMutex, 0);
        m_SynchronousEpochState = VioGpuSynchronousOffline;
        KeInitializeSpinLock(&m_NativeSubmitLock);
        InitializeListHead(&m_NativeSubmitBacklog);
        m_NativeSubmitBacklogPoisoned = 0;
        RtlZeroMemory(&m_LastNativeContextResponseDiagnostic, sizeof(m_LastNativeContextResponseDiagnostic));
    };

    PVOID AllocCmd(PGPU_VBUFFER *buf, int sz);
    PVOID AllocCmdResp(PGPU_VBUFFER *buf, int cmd_sz, PVOID resp_buf, int resp_sz);

    int QueueBuffer(PGPU_VBUFFER buf);
    PGPU_VBUFFER DequeueBuffer(_Out_ UINT *len);

    PGPU_VBUFFER PrepareNativeSubmit(UINT context_id, const void *command, UINT command_size);
    BOOLEAN RefreshNativeSubmit(PGPU_VBUFFER buf, const void *command, UINT command_size);
    int QueueNativeSubmit(PGPU_VBUFFER buf, ULONGLONG fence_id);
    void DrainNativeSubmitBacklog(void);
    /* Close the current transport generation to new submitters and wait for
     * any caller already inside QueueNativeSubmit/DrainNativeSubmitBacklog to
     * leave the software-queue critical section. */
    void PoisonNativeSubmitBacklog(void);
    void DetachNativeSubmitBacklog(void);
    /* Arm a newly initialized virtqueue after the previous generation was
     * detached.  A poisoned backlog must never silently carry into D0. */
    BOOLEAN ResetNativeSubmitBacklog(void);
    BOOLEAN QueryCapsetInfo(UINT capset_index, PGPU_RESP_CAPSET_INFO capset_info);
    BOOLEAN QueryCapset(UINT capset_id, UINT capset_version, UINT capset_size, PGPU_CAPSET_DRM capset);
    VIOGPU_HOST_CONTEXT_RESULT
    CreateNativeContext(UINT context_id, _Out_opt_ PVIOGPU_HOST_CONTEXT_RESPONSE_DIAGNOSTIC diagnostic = NULL);
    void GetLastNativeContextResponseDiagnostic(_Out_ PVIOGPU_HOST_CONTEXT_RESPONSE_DIAGNOSTIC diagnostic) const;
    VIOGPU_HOST_CONTEXT_RESULT DestroyNativeContext(UINT context_id);
    VIOGPU_HOST_CONTEXT_RESULT CreateNativeControlBlob(UINT context_id, UINT resource_id);
    VIOGPU_HOST_CONTEXT_RESULT CreateNativeGuestBlob(UINT context_id,
                                                     UINT resource_id,
                                                     UINT blob_id,
                                                     ULONGLONG size,
                                                     UINT blob_flags,
                                                     const GPU_MEM_ENTRY *entries,
                                                     UINT entry_count);
    VIOGPU_HOST_CONTEXT_RESULT MapNativeControlBlob(UINT resource_id, ULONGLONG offset);
    VIOGPU_HOST_CONTEXT_RESULT UnmapNativeControlBlob(UINT resource_id);
    VIOGPU_HOST_CONTEXT_RESULT UnrefNativeResource(UINT resource_id);
    VIOGPU_HOST_CONTEXT_RESULT SubmitNativeControl(UINT context_id, const void *command, UINT command_size);
    VIOGPU_HOST_CONTEXT_RESULT CreateResource2DSynchronous(UINT resource_id, UINT format, UINT width, UINT height);
    VIOGPU_HOST_CONTEXT_RESULT AttachBackingSynchronous(UINT resource_id,
                                                        const GPU_MEM_ENTRY *entries,
                                                        UINT entry_count);
    VIOGPU_HOST_CONTEXT_RESULT DetachBackingSynchronous(UINT resource_id);
    VIOGPU_HOST_CONTEXT_RESULT UnrefResourceSynchronous(UINT resource_id);
    VIOGPU_HOST_CONTEXT_RESULT SetScanoutSynchronous(UINT scanout_id,
                                                     UINT resource_id,
                                                     UINT width,
                                                     UINT height,
                                                     UINT x,
                                                     UINT y);
    VIOGPU_HOST_CONTEXT_RESULT TransferToHost2DSynchronous(UINT resource_id,
                                                           ULONGLONG offset,
                                                           UINT width,
                                                           UINT height,
                                                           UINT x,
                                                           UINT y);
    VIOGPU_HOST_CONTEXT_RESULT FlushResourceSynchronous(UINT resource_id, UINT width, UINT height, UINT x, UINT y);
    BOOLEAN EnableSynchronousRequests(void);
    BOOLEAN IsSynchronousRequestsHealthy(void);
    NTSTATUS QuiesceSynchronousRequests(void);
    void CompleteSynchronousRequestTeardown(void);
    void PoisonSynchronousRequests(void);

    void CreateResource(UINT res_id, UINT format, UINT width, UINT height);
    void DestroyResource(UINT id);
    void SetScanout(UINT scan_id, UINT res_id, UINT width, UINT height, UINT x, UINT y);
    void ResFlush(UINT res_id, UINT width, UINT height, UINT x, UINT y);
    void TransferToHost2D(UINT res_id, ULONG offset, UINT width, UINT height, UINT x, UINT y);
    void AttachBacking(UINT res_id, PGPU_MEM_ENTRY ents, UINT nents);
    void DetachBacking(UINT id);

    BOOLEAN QueryDisplayInfo(UINT id, _Out_ PULONG xres, _Out_ PULONG yres);
    BOOLEAN QueryEdidInfo(UINT id, _Out_writes_bytes_(EDID_RAW_BLOCK_SIZE) PBYTE edid);

  private:
    BOOLEAN BeginSynchronousRequest(void);
    void EndSynchronousRequest(void);
    BOOLEAN SubmitSynchronousLocked(PGPU_VBUFFER buf, _Out_ PBOOLEAN release_buffer);
    BOOLEAN SubmitSynchronousLocked(PGPU_VBUFFER buf, _Out_ PBOOLEAN release_buffer, _Out_ PBOOLEAN submitted);
    VIOGPU_HOST_CONTEXT_RESULT SubmitSynchronousNoDataLocked(PGPU_VBUFFER buf);
    KMUTEX m_SynchronousMutex;
    DECLSPEC_ALIGN(8) volatile LONG64 m_SynchronousEpochState;
    volatile LONG m_FenceIdr;
    KSPIN_LOCK m_NativeSubmitLock;
    LIST_ENTRY m_NativeSubmitBacklog;
    volatile LONG m_NativeSubmitBacklogPoisoned;
    VIOGPU_HOST_CONTEXT_RESPONSE_DIAGNOSTIC m_LastNativeContextResponseDiagnostic;
};

class CrsrQueue : public VioGpuQueue
{
  public:
    PVOID AllocCursor(PGPU_VBUFFER *buf);
    UINT QueueCursor(PGPU_VBUFFER buf);
    PGPU_VBUFFER DequeueCursor(_Out_ UINT *len);
};
