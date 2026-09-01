/*
 * Copyright (C) 2019-2022 Red Hat, Inc.
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
#include "viogpu_queue.h"

#pragma pack(push)
#pragma pack(1)

typedef struct
{
    UINT DriverStarted : 1;
    UINT HardwareInit : 1;
    UINT PointerEnabled : 1;
    UINT VgaDevice : 1;
    UINT FlexResolution : 1;
    UINT UsePhysicalMemory : 1;
    UINT UsePresentProgress : 1;
    UINT RenderOnly : 1;
    UINT Unused : 24;
} DRIVER_STATUS_FLAG;

#pragma pack(pop)

typedef struct _CURRENT_MODE
{
    DXGK_DISPLAY_INFORMATION DispInfo;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation;
    D3DKMDT_VIDPN_PRESENT_PATH_SCALING Scaling;
    UINT SrcModeWidth;
    UINT SrcModeHeight;
    struct _CURRENT_MODE_FLAGS
    {
        UINT SourceNotVisible : 1;
        UINT FullscreenPresent : 1;
        UINT FrameBufferIsActive : 1;
        UINT DoNotMapOrUnmap : 1;
        UINT IsInternal : 1;
        UINT Unused : 27;
    } Flags;

    PVOID FrameBuffer;
    PVOID RamFrameBuffer;
} CURRENT_MODE;

class VioGpuDod;

enum VIOGPU_NATIVE_START_DETAIL : DWORD
{
    VioGpuNativeStartDetailNone = 0,
    VioGpuNativeStartDetailMissingVirgl = 1U << 0,
    VioGpuNativeStartDetailMissingResourceBlob = 1U << 1,
    VioGpuNativeStartDetailMissingContextInit = 1U << 2,
    VioGpuNativeStartDetailMissingGuestHandle = 1U << 3,
    VioGpuNativeStartDetailInvalidWireVersion = 1U << 8,
    VioGpuNativeStartDetailInvalidContextType = 1U << 9,
    VioGpuNativeStartDetailInvalidPadding = 1U << 10,
    VioGpuNativeStartDetailInvalidMsmVersion = 1U << 11,
    VioGpuNativeStartDetailInvalidPriorities = 1U << 12,
    VioGpuNativeStartDetailInvalidVaStart = 1U << 13,
    VioGpuNativeStartDetailInvalidVaSize = 1U << 14,
    VioGpuNativeStartDetailInvalidVaRange = 1U << 15,
};

#if defined(VIOGPU_NATIVE_CONTEXT)
enum : UINT
{
    VioGpuNativeFenceTrackerCapacity = 4096,
    VioGpuNativeContextDestroyDiagnosticSlotCount = 64,
};

enum VIOGPU_NATIVE_START_STAGE : DWORD
{
    VioGpuNativeStartEntered = 0x0100,
    VioGpuNativeStartPreconditions = 0x0110,
    VioGpuNativeStartDeviceInformation = 0x0120,
    VioGpuNativeStartHardwareIdentity = 0x0130,
    VioGpuNativeStartAdapterAllocation = 0x0140,
    VioGpuNativeStartRegistryConfiguration = 0x0150,
    VioGpuNativeStartBeginInitialization = 0x0200,
    VioGpuNativeStartPciResources = 0x0210,
    VioGpuNativeStartVirtioPreconditions = 0x0300,
    VioGpuNativeStartVirtioDevice = 0x0310,
    VioGpuNativeStartVirtioVersion = 0x0320,
    VioGpuNativeStartVirtioNativeFeatures = 0x0330,
    VioGpuNativeStartVirtioSetFeatures = 0x0340,
    VioGpuNativeStartVirtioFindQueues = 0x0350,
    VioGpuNativeStartVirtioQueueObjects = 0x0360,
    VioGpuNativeStartVirtioQueueBacklog = 0x0370,
    VioGpuNativeStartVirtioConfig = 0x0380,
    VioGpuNativeStartHostVisibleRegion = 0x0400,
    VioGpuNativeStartQueueBuffer = 0x0410,
    VioGpuNativeStartResourceIds = 0x0420,
    VioGpuNativeStartQueueInterrupts = 0x0430,
    VioGpuNativeStartDriverReady = 0x0440,
    VioGpuNativeStartSynchronousRequests = 0x0450,
    VioGpuNativeStartCapsetFeatureState = 0x0500,
    VioGpuNativeStartCapsetCount = 0x0510,
    VioGpuNativeStartCapsetInfoQuery = 0x0520,
    VioGpuNativeStartCapsetInfoUnique = 0x0530,
    VioGpuNativeStartCapsetInfoLayout = 0x0540,
    VioGpuNativeStartCapsetPayloadQuery = 0x0550,
    VioGpuNativeStartCapsetPayloadValidation = 0x0560,
    VioGpuNativeStartCapsetPublish = 0x0570,
    VioGpuNativeStartModeList = 0x0600,
    VioGpuNativeStartFrameSegment = 0x0610,
    VioGpuNativeStartCursorSegment = 0x0620,
    VioGpuNativeStartWorkThread = 0x0700,
    VioGpuNativeStartCompleteInitialization = 0x0710,
    VioGpuNativeStartHardwareInformation = 0x0800,
    VioGpuNativeStartPostDisplayOwnership = 0x0810,
    VioGpuNativeStartFinalState = 0x0820,
    VioGpuNativeStartComplete = 0x0FFF,
};

enum VIOGPU_NATIVE_CONTEXT_CREATE_STAGE : DWORD
{
    VioGpuNativeContextCreateEntered = 0x0100,
    VioGpuNativeContextCreatePreconditions = 0x0110,
    VioGpuNativeContextCreateMutex = 0x0120,
    VioGpuNativeContextCreateReadiness = 0x0130,
    VioGpuNativeContextCreateIds = 0x0140,
    VioGpuNativeContextCreateOwner = 0x0150,
    VioGpuNativeContextCreateHostContext = 0x0200,
    VioGpuNativeContextCreateControlBlob = 0x0210,
    VioGpuNativeContextCreateControlMap = 0x0220,
    VioGpuNativeContextCreateVaStart = 0x0230,
    VioGpuNativeContextCreateVaSize = 0x0240,
    VioGpuNativeContextCreateSubmitQueue = 0x0250,
    VioGpuNativeContextCreateCurrent = 0x0300,
    VioGpuNativeContextCreateComplete = 0x0FFF,
};

enum VIOGPU_NATIVE_CONTEXT_DESTROY_STAGE : DWORD
{
    VioGpuNativeContextDestroyEntered = 0x0100,
    VioGpuNativeContextDestroyRundown = 0x0110,
    VioGpuNativeContextDestroyBusy = 0x0120,
    VioGpuNativeContextDestroyAdapter = 0x0130,
    VioGpuNativeContextDestroyMarked = 0x0140,
    VioGpuNativeContextDestroyHostBegin = 0x0200,
    VioGpuNativeContextDestroyHostResult = 0x0210,
    VioGpuNativeContextDestroyRetired = 0x0300,
    VioGpuNativeContextDestroyComplete = 0x0FFF,
};

enum VIOGPU_NATIVE_ALLOCATION_DESTROY_STAGE : DWORD
{
    VioGpuNativeAllocationDestroyEntered = 0x0100,
    VioGpuNativeAllocationDestroyLifecycle = 0x0110,
    VioGpuNativeAllocationDestroyBegin = 0x0120,
    VioGpuNativeAllocationDestroyHost = 0x0130,
    VioGpuNativeAllocationDestroyDetach = 0x0140,
    VioGpuNativeAllocationDestroyComplete = 0x0FFF,
};

struct VIOGPU_NATIVE_PRESENT_DIAGNOSTIC
{
    DWORD ContextType;
    DWORD PresentFlags;
    DWORD SubRectCount;
    DWORD MultipassOffset;
    DWORD SourceFlags;
    DWORD DestinationFlags;
    DWORD SourceHostState;
    DWORD DestinationHostState;
    DWORD SourceResource2DState;
    DWORD DestinationResource2DState;
    DWORD SourcePlacementState;
    DWORD DestinationPlacementState;
    DWORD SourceFormat;
    DWORD DestinationFormat;
    DWORD SourceWidth;
    DWORD SourceHeight;
    DWORD SourcePitch;
    DWORD DestinationWidth;
    DWORD DestinationHeight;
    DWORD DestinationPitch;
    DWORD SourceAllocationListValue;
    DWORD DestinationAllocationListValue;
    DWORD SourceResourceId;
    DWORD DestinationResourceId;
    DWORD SourceRectLeft;
    DWORD SourceRectTop;
    DWORD SourceRectRight;
    DWORD SourceRectBottom;
    DWORD DestinationRectLeft;
    DWORD DestinationRectTop;
    DWORD DestinationRectRight;
    DWORD DestinationRectBottom;
};

struct VIOGPU_NATIVE_PRESENT_EXECUTION_DIAGNOSTIC
{
    DWORD Stage;
    DWORD Status;
    DWORD Detail;
    DWORD FenceId;
    DWORD TransactionState;
    DWORD ContextType;
    DWORD SourceResourceId;
    DWORD DestinationResourceId;
    DWORD SourcePlacementState;
    DWORD DestinationPlacementState;
    DWORD SourceResource2DState;
    DWORD DestinationResource2DState;
    DWORD SourcePlacementOffsetLow;
    DWORD SourcePlacementOffsetHigh;
    DWORD DestinationPlacementOffsetLow;
    DWORD DestinationPlacementOffsetHigh;
    DWORD TransactionSourcePlacementOffsetLow;
    DWORD TransactionSourcePlacementOffsetHigh;
    DWORD TransactionDestinationPlacementOffsetLow;
    DWORD TransactionDestinationPlacementOffsetHigh;
    DWORD SourceResetGenerationLow;
    DWORD SourceResetGenerationHigh;
    DWORD DestinationResetGenerationLow;
    DWORD DestinationResetGenerationHigh;
    DWORD TransactionDestinationResetGenerationLow;
    DWORD TransactionDestinationResetGenerationHigh;
};

struct VIOGPU_NATIVE_PRESENT_COPY_PROBE
{
    DWORD FenceId;
    DWORD SampleCount;
    DWORD SourceRgbNonzero;
    DWORD DestinationRgbNonzero;
    DWORD SourceHash;
    DWORD DestinationHash;
    DWORD SourceFirstPixel;
    DWORD DestinationFirstPixel;
    DWORD SourceResourceId;
    DWORD DestinationResourceId;
    DWORD RectCount;
    DWORD HostPresentCount;
    DWORD HostPresentResult;
};

enum VIOGPU_NATIVE_FENCE_STATE : LONG
{
    VioGpuNativeFenceFree = 0,
    VioGpuNativeFencePending,
    VioGpuNativeFenceSoftwarePending,
    VioGpuNativeFenceRetired,
};

struct VIOGPU_NATIVE_FENCE_ENTRY
{
    UINT FenceId;
    VIOGPU_NATIVE_FENCE_STATE State;
};

typedef VOID (*VIOGPU_NATIVE_PASSIVE_ROUTINE)(_In_ PVOID context);

enum VIOGPU_NATIVE_PASSIVE_WORK_STATE : LONG
{
    VioGpuNativePassiveWorkIdle = 0,
    VioGpuNativePassiveWorkQueued,
    VioGpuNativePassiveWorkWorkerOwned,
};

enum VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIP : LONG
{
    VioGpuNativePassiveWorkNotQueued = 0,
    VioGpuNativePassiveWorkRemoved,
    VioGpuNativePassiveOwnershipWorkerOwned,
};

struct VIOGPU_NATIVE_PASSIVE_WORK
{
    LIST_ENTRY Link;
    VIOGPU_NATIVE_PASSIVE_ROUTINE Routine;
    VIOGPU_NATIVE_PASSIVE_ROUTINE CancelRoutine;
    PVOID Context;
    volatile LONG *CancelRequested;
    UINT FenceId;
    volatile LONG State;
    volatile LONG Retired;
};
#endif
class VioGpuAdapter;
struct VIOGPU_NATIVE_CONTEXT_REGISTRATION;

enum VIOGPU_NATIVE_CONTEXT_OWNER_STATE : LONG
{
    VioGpuNativeContextOwnerCreating = 0,
    VioGpuNativeContextOwnerLive,
    VioGpuNativeContextOwnerDestroying,
};

struct VIOGPU_NATIVE_CONTEXT_OWNER
{
    LIST_ENTRY AdapterLink;
    VIOGPU_NATIVE_CONTEXT_REGISTRATION *Registration;
    VIOGPU_NATIVE_CONTEXT_OWNER_STATE State;
    LONG Generation;
    ULONGLONG ResetGeneration;
    UINT ContextId;
    volatile LONG AllocationCount;
#if defined(VIOGPU_NATIVE_CONTEXT)
    UINT ControlResourceId;
    ULONGLONG ControlBarOffset;
    PVOID ControlAddress;
    ULONG ControlBlobSize;
    ULONG LastControlSeqno;
    UINT SubmitQueueId;
    BOOLEAN ControlResourceCreated;
    BOOLEAN ControlMapped;
    BOOLEAN SubmitQueueCreated;
    UINT ControlHostVisibleBar;
    ULONG ControlMapStatus;
    ULONGLONG ControlHostVisibleOffset;
    ULONGLONG ControlHostVisibleSize;
#endif
};

enum VIOGPU_NATIVE_CONTEXT_OBJECT_STATE : LONG
{
    VioGpuNativeContextAllocated = 0,
    VioGpuNativeContextCreating,
    VioGpuNativeContextLive,
    VioGpuNativeContextDestroying,
    VioGpuNativeContextDead,
};

struct VIOGPU_NATIVE_CONTEXT_REGISTRATION
{
    KSPIN_LOCK BindingLock;
    VioGpuAdapter *Adapter;
    VIOGPU_NATIVE_CONTEXT_OWNER *Owner;
    volatile LONG State;
    LONG Generation;
    ULONGLONG ResetGeneration;
    UINT ContextId;
    ULONGLONG VaStart;
    ULONGLONG VaSize;
    UINT SubmitQueueId;
    ULONG AllocationReferences;
    LIST_ENTRY AllocationRanges;
    BOOLEAN Registered;
    BOOLEAN AllocationClosing;
};

struct VIOGPU_NATIVE_CONTEXT_SNAPSHOT
{
    VioGpuAdapter *Adapter;
    VIOGPU_NATIVE_CONTEXT_OWNER *Owner;
    VIOGPU_NATIVE_CONTEXT_REGISTRATION *Registration;
    LONG Generation;
    ULONGLONG ResetGeneration;
    UINT ContextId;
    ULONGLONG VaStart;
    ULONGLONG VaSize;
    UINT SubmitQueueId;
};

enum VIOGPU_NATIVE_CONTEXT_STATE : LONG
{
    VioGpuNativeContextOffline = 0,
    VioGpuNativeContextStarting,
    VioGpuNativeContextReady,
    VioGpuNativeContextQuiescing,
    VioGpuNativeContextFailed,
};

enum VIOGPU_HARDWARE_RESET_STATE : LONG
{
    VioGpuHardwareActive = 0,
    VioGpuHardwareResetRequested,
    VioGpuHardwareRecovering,
};

struct VIOGPU_NATIVE_CONTEXT_READINESS
{
    BOOLEAN Ready;
    LONG Generation;
    ULONGLONG ResetGeneration;
    UINT CapsetVersion;
    UINT CapsetSize;
    GPU_CAPSET_DRM Capset;
};

class VioGpuAdapter : IVioGpuPCI
{
  public:
    VioGpuAdapter(_In_ VioGpuDod *pVioGpuDod);
    ~VioGpuAdapter(void);
    NTSTATUS SetCurrentMode(ULONG Mode, CURRENT_MODE *pCurrentMode);
    ULONG GetModeCount(void)
    {
        return m_ModeCount;
    }
    NTSTATUS SetPowerState(DXGK_DEVICE_INFO *pDeviceInfo,
                           DEVICE_POWER_STATE DevicePowerState,
                           CURRENT_MODE *pCurrentMode);
    NTSTATUS HWInit(PCM_RESOURCE_LIST pResList, DXGK_DISPLAY_INFORMATION *pDispInfo);
    NTSTATUS HWClose(void);
    NTSTATUS ControlInterrupt(_In_ BOOLEAN enableInterrupt);
    NTSTATUS ExecutePresentDisplayOnly(_In_ BYTE *DstAddr,
                                       _In_ UINT DstBitPerPixel,
                                       _In_ BYTE *SrcAddr,
                                       _In_ UINT SrcBytesPerPixel,
                                       _In_ LONG SrcPitch,
                                       _In_ ULONG NumMoves,
                                       _In_ D3DKMT_MOVE_RECT *pMoves,
                                       _In_ ULONG NumDirtyRects,
                                       _In_ RECT *pDirtyRect,
                                       _In_ D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation,
                                       _In_ const CURRENT_MODE *pModeCur);
    VOID BlackOutScreen(CURRENT_MODE *pCurrentMod);
    BOOLEAN InterruptRoutine(_In_ PDXGKRNL_INTERFACE pDxgkInterface, _In_ ULONG MessageNumber);
    VOID DpcRoutine(_In_ PDXGKRNL_INTERFACE pDxgkInterface);
    VOID ResetDevice(VOID);
    NTSTATUS SetPointerShape(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape, _In_ CONST CURRENT_MODE *pModeCur);
    NTSTATUS SetPointerPosition(_In_ CONST DXGKARG_SETPOINTERPOSITION *pSetPointerPosition,
                                _In_ CONST CURRENT_MODE *pModeCur);
    NTSTATUS Escape(_In_ CONST DXGKARG_ESCAPE *pEscap);
    /* May be called by the display-only transport completion path at any IRQL. */
    void FailNativeContextAtAnyIrql(void);
    CPciResources *GetPciResources(void)
    {
        return &m_PciResources;
    }
    BOOLEAN ResetToVgaMode(void);
    BOOLEAN IsMSIEnabled()
    {
        return m_PciResources.IsMSIEnabled();
    }
    PHYSICAL_ADDRESS GetFrameBufferPA(void)
    {
        return m_PciResources.GetPciBar(0)->GetPA();
    }
    SIZE_T GetFrameSegmentSize(void)
    {
        return m_FrameSegment.GetSize();
    }
    PDXGKRNL_INTERFACE GetDxgkInterface(void);
#if defined(VIOGPU_NATIVE_CONTEXT)
    /* Independent from the outer device rundown: D-state transitions keep
     * m_HardwareOperations open, but must still quiesce every WDDM native
     * submitter before the transport resets/deletes its virtqueues. */
    __declspec(code_seg(".text")) BOOLEAN AcquireNativeSubmitOperation(void) const;
    void ReleaseNativeSubmitOperation(void) const;
    __declspec(code_seg(".text")) void CompleteNativeSubmitRundown(void);
    __declspec(code_seg(".text")) BOOLEAN ReinitializeNativeSubmitRundown(void);
    UINT Allocate2DResourceId(void);
    BOOLEAN Release2DResourceId(_In_ UINT resourceId);
    VIOGPU_HOST_CONTEXT_RESULT Create2DResourceBacking(_In_ UINT resourceId,
                                                       _In_ UINT format,
                                                       _In_ UINT width,
                                                       _In_ UINT height,
                                                       _In_ SIZE_T backingSize,
                                                       _In_reads_(entryCount) const GPU_MEM_ENTRY *entries,
                                                       _In_ UINT entryCount,
                                                       _Inout_ VIOGPU_2D_RESOURCE_STATE *resourceState,
                                                       _Inout_ ULONGLONG *resourceResetGeneration);
    VIOGPU_HOST_CONTEXT_RESULT Destroy2DResource(_In_ UINT resourceId,
                                                 _Inout_ VIOGPU_2D_RESOURCE_STATE *resourceState,
                                                 _Inout_ ULONGLONG *resourceResetGeneration,
                                                 _Out_ BOOLEAN *released);
    BOOLEAN Reconcile2DResourceAfterReset(_Inout_ VIOGPU_2D_RESOURCE_STATE *resourceState,
                                          _Inout_ ULONGLONG *resourceResetGeneration,
                                          _Out_ BOOLEAN *retired);
    VIOGPU_HOST_CONTEXT_RESULT Present2DResource(_In_ UINT resourceId,
                                                 _In_ ULONGLONG offset,
                                                 _In_ UINT width,
                                                 _In_ UINT height,
                                                 _In_ UINT x,
                                                 _In_ UINT y,
                                                 _Inout_ VIOGPU_2D_RESOURCE_STATE *resourceState,
                                                 _Inout_ ULONGLONG *resourceResetGeneration);
    VIOGPU_HOST_CONTEXT_RESULT Set2DScanout(_In_ UINT scanoutId,
                                            _In_ UINT resourceId,
                                            _In_ UINT width,
                                            _In_ UINT height,
                                            _Out_ UINT *previousResourceId);
    VIOGPU_HOST_CONTEXT_RESULT Detach2DScanoutResource(_In_ UINT resourceId, _Out_ BOOLEAN *detached);
    BOOLEAN Query2DScanoutResource(_In_ UINT resourceId, _Out_ BOOLEAN *active);
    UINT AllocateNativeResourceId(_In_ ULONGLONG expectedResetGeneration);
    VIOGPU_HOST_CONTEXT_RESULT CreateNativeGuestAllocation(_In_ const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot,
                                                           _In_ UINT resourceId,
                                                           _In_ UINT blobId,
                                                           _In_ ULONGLONG logicalSize,
                                                           _In_ SIZE_T backingSize,
                                                           _In_ ULONGLONG requestedIova,
                                                           _In_reads_(entryCount) const GPU_MEM_ENTRY *entries,
                                                           _In_ UINT entryCount,
                                                           _In_ UINT msmFlags,
                                                           _In_ UINT blobFlags,
                                                           _Out_ BOOLEAN *ownershipRetained);
    VIOGPU_HOST_CONTEXT_RESULT DestroyNativeGuestAllocation(_In_ const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot,
                                                            _In_ UINT resourceId,
                                                            _Out_ BOOLEAN *released);
    BOOLEAN IsNativeContextResetRetired(_In_ ULONGLONG resetGeneration);
#endif
    _IRQL_requires_max_(DISPATCH_LEVEL) BOOLEAN QueryNativeContextReadiness(_Out_ PGPU_CAPSET_DRM capset,
                                                                            _Out_opt_ UINT *capsetVersion,
                                                                            _Out_opt_ UINT *capsetSize,
                                                                            _Out_opt_ ULONGLONG *resetGeneration);
    __declspec(code_seg(".text")) NTSTATUS CreateNativeContext(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context,
                                                               _In_ ULONGLONG expectedResetGeneration);
    __declspec(code_seg(".text")) NTSTATUS DestroyNativeContext(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context,
                                                                _Out_ BOOLEAN *released);
    __declspec(code_seg(".text")) static BOOLEAN AcquireNativeContextSnapshot(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context,
                                                                              _Out_ VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot);
    __declspec(code_seg(".text")) BOOLEAN AcquireNativeContextSnapshotForAllocation(_In_ ULONGLONG requestedIova,
                                                                                    _In_ SIZE_T backingSize,
                                                                                    _In_ ULONGLONG expectedResetGeneration,
                                                                                    _In_ UINT expectedContextId,
                                                                                    _Out_ VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot);
    static BOOLEAN ReferenceNativeContextAllocation(_In_ const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot,
                                                    _Out_ VIOGPU_NATIVE_CONTEXT_REGISTRATION **registration);
    static BOOLEAN DereferenceNativeContextAllocation(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *registration);
    static BOOLEAN IsNativeContextAllocationBindingRetired(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *registration);
    static void ReleaseNativeContextSnapshot(_Inout_ VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot);
    static VioGpuAdapter *ReferenceNativeContextAdapter(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context);
    static void DereferenceNativeContextAdapter(_In_ VioGpuAdapter *adapter);
    static BOOLEAN IsNativeContextReleased(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context);
    BOOLEAN IsNativeContextGenerationCurrent(_In_ LONG generation, _In_ ULONGLONG resetGeneration);
#if defined(VIOGPU_NATIVE_CONTEXT)
    PGPU_VBUFFER PrepareNativeSubmit(_In_ UINT contextId, _In_ const void *command, _In_ UINT commandSize)
    {
        return m_CtrlQueue.PrepareNativeSubmit(contextId, command, commandSize);
    }
    BOOLEAN RefreshNativeSubmit(_In_ PGPU_VBUFFER buffer, _In_ const void *command, _In_ UINT commandSize)
    {
        return m_CtrlQueue.RefreshNativeSubmit(buffer, command, commandSize);
    }
    int QueueNativeSubmit(_In_ PGPU_VBUFFER buffer, _In_ ULONGLONG fenceId)
    {
        return m_CtrlQueue.QueueNativeSubmit(buffer, fenceId);
    }
    void ReleaseNativeSubmitBuffer(_In_ PGPU_VBUFFER buffer)
    {
        m_CtrlQueue.ReleaseBuffer(buffer);
    }
    BOOLEAN QueryNativeSubmitInterruptMessage(_Out_ ULONG *messageNumber)
    {
        if (messageNumber == NULL || !m_PciResources.HasKnownInterruptMessageCount())
        {
            return FALSE;
        }
        ULONG selected = m_PciResources.IsMSIEnabled() ? 1U : 0U;
        if (selected >= m_PciResources.GetInterruptMessageCount())
        {
            return FALSE;
        }
        *messageNumber = selected;
        return TRUE;
    }
#endif

    PVIDEO_MODE_INFORMATION GetModeInfo(UINT idx)
    {
        return &m_ModeInfo[idx];
    }
    USHORT GetModeNumber(USHORT idx)
    {
        return (USHORT)m_ModeInfo[idx].ModeIndex;
    }
    USHORT GetCurrentModeIndex(void)
    {
        return m_CurrentModeIndex;
    }
    VOID SetCurrentModeIndex(USHORT idx)
    {
        m_CurrentModeIndex = idx;
    }
    VioGpuDod *GetVioGpu(void)
    {
        return m_pVioGpuDod;
    }
    ULONG GetInstanceId(void)
    {
        return m_Id;
    }
    PBYTE GetEdidData(void);
    PBYTE GetCTA861Data(void);

  protected:
  private:
    NTSTATUS VioGpuAdapterInit(DXGK_DISPLAY_INFORMATION *pDispInfo);
    void SetVideoModeInfo(UINT Idx, PVIOGPU_DISP_MODE pModeInfo);
    NTSTATUS StopNativeContextTransport(void);
    NTSTATUS StopNativeContextTransportLocked(void);
    NTSTATUS SynchronizeInterruptMessages(void);
    __declspec(code_seg(".text")) void InvalidateNativeContextRegistrationsLocked(void);
    NTSTATUS RetireAllNativeContextOwnersLocked(void);
    void RetireNativeContextOwnerLocked(_Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner);
    void Publish2DResetRetirementLocked(void);
    void Reconcile2DScanoutAfterResetLocked(void);
    UINT AllocateNativeContextIdLocked(void);
#if defined(VIOGPU_NATIVE_CONTEXT)
    UINT AllocateNativeResourceIdLocked(void);
    VIOGPU_HOST_CONTEXT_RESULT QueryNativeContextParameterLocked(_Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner,
                                                                 _In_ ULONG parameter,
                                                                 _Out_ PULONGLONG value);
    VIOGPU_HOST_CONTEXT_RESULT CreateNativeSubmitQueueLocked(_Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner,
                                                             _Out_ PUINT queueId);
    VIOGPU_HOST_CONTEXT_RESULT CloseNativeSubmitQueueLocked(_Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner);
    VIOGPU_HOST_CONTEXT_RESULT DestroyNativeContextHostObjectsLocked(_Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner);
#endif
    BOOLEAN BeginNativeContextInitialization(void);
    __declspec(code_seg(".text")) BOOLEAN CompleteNativeContextInitialization(void);
    NTSTATUS NegotiateNativeContextFeatures(void);
    __declspec(code_seg(".text")) NTSTATUS ProbeNativeContextReadiness(void);
#if defined(VIOGPU_NATIVE_CONTEXT)
    BOOLEAN AllocateNativeControlSlotLocked(_Out_ PULONGLONG offset, _Out_ PVOID *address);
#endif
    NTSTATUS StartNativeContextTransport(DXGK_DISPLAY_INFORMATION *pDispInfo);
    NTSTATUS FailNativeContextInitialization(NTSTATUS status);
    NTSTATUS StartWorkThread(void);
    NTSTATUS StopWorkThread(void);
    void ClearNativeContextReadiness(void);
    NTSTATUS BuildModeList(DXGK_DISPLAY_INFORMATION *pDispInfo);
    BOOLEAN AckFeature(UINT64 Feature);
    BOOLEAN GetDisplayInfo(void);
    int ProcessEdid(void);
    void FixEdid(void);
    BOOLEAN GetEdids(void);
    int AddEdidModes(void);
    BOOLEAN UpdateModes(USHORT xres, USHORT yres, int &cnt);
    NTSTATUS UpdateChildStatus(BOOLEAN connect);
    void SetCustomDisplay(_In_ USHORT xres, _In_ USHORT yres);
    BOOLEAN CreateFrameBufferObj(PVIDEO_MODE_INFORMATION pModeInfo, CURRENT_MODE *pCurrentMode);
    void DestroyFrameBufferObj(BOOLEAN bReset, BOOLEAN bKeepBuffer);
    BOOLEAN CreateCursor(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape, _In_ CONST CURRENT_MODE *pCurrentMode);
    BOOLEAN UpdateCursor(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape, _In_ CONST CURRENT_MODE *pCurrentMode);
    void DestroyCursor(void);
    BOOLEAN GpuObjectAttach(UINT res_id, VioGpuObj *obj);
    void static ThreadWork(_In_ PVOID Context);
    void ThreadWorkRoutine(void);
    void ConfigChanged(void);
    NTSTATUS VirtIoDeviceInit(void);
    VOID CreateResolutionEvent(VOID);
    VOID NotifyResolutionEvent(VOID);
    VOID CloseResolutionEvent(VOID);

  private:
    VioGpuDod *m_pVioGpuDod;
    PVIDEO_MODE_INFORMATION m_ModeInfo;
    ULONG m_ModeCount;
    USHORT m_CurrentModeIndex;
    USHORT m_CustomModeIndex;
    ULONG m_Id;
    BYTE m_EDIDs[EDID_RAW_BLOCK_SIZE];
    BOOLEAN m_bEDID;

    VirtIODevice m_VioDev;
    CPciResources m_PciResources;
    UINT64 m_u64HostFeatures;
    UINT64 m_u64GuestFeatures;
    UINT32 m_u32NumCapsets;
    UINT32 m_u32NumScanouts;
    KSPIN_LOCK m_NativeContextReadinessLock;
    VIOGPU_NATIVE_CONTEXT_READINESS m_NativeContextReadiness;
    KMUTEX m_NativeContextLifecycleMutex;
    LIST_ENTRY m_NativeContextRegistry;
    EX_RUNDOWN_REF m_NativeContextReferences;
    UINT m_NextNativeContextId;
#if defined(VIOGPU_NATIVE_CONTEXT)
    UINT m_NextNativeResourceId;
    KMUTEX m_2DScanoutMutex;
    BOOLEAN m_2DResourceIdsInitialized;
    UINT m_2DScanoutResourceId;
    BOOLEAN m_2DScanoutUnknown;
    ULONGLONG m_2DScanoutResetGeneration;
    DECLSPEC_ALIGN(8) volatile LONG64 m_2DRetiredResetGeneration;
    mutable KSPIN_LOCK m_NativeSubmitRundownLock;
    mutable EX_RUNDOWN_REF m_NativeSubmitRundown;
    BOOLEAN m_NativeSubmitClosing;
    BOOLEAN m_NativeSubmitRundownCompleted;
#endif
    volatile LONG m_NativeContextState;
    volatile LONG m_NativeContextGeneration;
    DECLSPEC_ALIGN(8) volatile LONG64 m_NativeContextResetGeneration;
    volatile LONG m_InterruptDispatchEnabled;
    BOOLEAN m_bVirtioInitialized;
    BOOLEAN m_bQueuesInitialized;
    HANDLE m_WorkThreadHandle;
    CtrlQueue m_CtrlQueue;
    CrsrQueue m_CursorQueue;
    VioGpuBuf m_GpuBuf;
    VioGpuIdr m_Idr;
    VioGpuObj *m_pFrameBuf;
    VioGpuObj *m_pCursorBuf;
    VioGpuMemSegment m_CursorSegment;
    VioGpuMemSegment m_FrameSegment;
    volatile ULONG m_PendingWorks;
    KEVENT m_ConfigUpdateEvent;
    PETHREAD m_pWorkThread;
    BOOLEAN m_bStopWorkThread;
    PKEVENT m_ResolutionEvent;
    HANDLE m_ResolutionEventHandle;
};

class VioGpuDod
{
  private:
    DEVICE_OBJECT *m_pPhysicalDevice;
    DXGKRNL_INTERFACE m_DxgkInterface;
    DXGK_DEVICE_INFO m_DeviceInfo;

    DEVICE_POWER_STATE m_MonitorPowerState;
    DEVICE_POWER_STATE m_AdapterPowerState;
    DRIVER_STATUS_FLAG m_Flags;

    CURRENT_MODE m_CurrentMode;

    DXGK_DISPLAY_INFORMATION m_SystemDisplayInfo;

    DXGKARG_SETPOINTERSHAPE m_PointerShape;
    VioGpuAdapter *m_pHWDevice;
    mutable EX_RUNDOWN_REF m_HardwareOperations;
    BOOLEAN m_HardwareRundownCompleted;
    mutable volatile LONG m_HardwareResetState;
#if defined(VIOGPU_NATIVE_CONTEXT)
    volatile LONG m_HardwareResetCallerRva;
    volatile LONG m_NativeSubmissionFaultDiagnosticRecorded;
    volatile LONG m_NativeSubmissionFaultCallerRva;
    volatile LONG m_NativeSubmissionFaultExecutionDiagnosticState;
    volatile LONG m_NativeSubmissionFaultPresentSubmitStage;
    volatile LONG m_NativeSubmissionFaultPresentSubmitStatus;
    volatile LONG m_NativeSubmissionFaultPresentSubmitDetail;
    volatile LONG m_NativeContextDestroyAttempt;
    KMUTEX m_NativeContextDestroyDiagnosticMutex;
    KSPIN_LOCK m_NativeFenceLock;
    UINT m_NativeFenceHead;
    UINT m_NativeFenceCount;
    VIOGPU_NATIVE_FENCE_ENTRY m_NativeFences[VioGpuNativeFenceTrackerCapacity];
    volatile LONG m_NativeSubmittedFence;
    volatile LONG m_NativeCompletedFence;
    KSPIN_LOCK m_NativePassiveLock;
    LIST_ENTRY m_NativePassiveQueue;
    WORK_QUEUE_ITEM m_NativePassiveWorkItem;
    BOOLEAN m_NativePassiveWorkerQueued;
    VIOGPU_NATIVE_PASSIVE_WORK *m_NativePassiveActiveWork;
    volatile LONG m_NativePassiveClosing;
    KEVENT m_NativePassiveIdleEvent;
    WORK_QUEUE_ITEM m_WddmDrainWorkItem;
    volatile LONG m_WddmDrainWorkerQueued;
    volatile LONG m_WddmDrainRequested;
    KEVENT m_WddmDrainIdleEvent;
    KSPIN_LOCK m_WddmPresentLock;
    LIST_ENTRY m_WddmPresentTransactions;
    volatile LONG m_WddmPresentClosing;
    volatile LONG m_NativePresentDiagnosticRecorded;
    volatile LONG m_NativePresentExecutionDiagnosticRecorded;
    volatile LONG m_NativePresentCopyProbeState;
    volatile LONG m_NativePresentCopyProbeSequence;
#endif

    USHORT m_PersistentDispMode0Width;
    USHORT m_PersistentDispMode0Height;

  public:
    VioGpuDod(_In_ DEVICE_OBJECT *pPhysicalDeviceObject);
    ~VioGpuDod(void);
#pragma code_seg(push)
#pragma code_seg()
    BOOLEAN IsDriverActive() const
    {
        return m_Flags.DriverStarted;
    }
    BOOLEAN IsHardwareInit() const
    {
        return m_Flags.HardwareInit;
    }
    void SetHardwareInit(BOOLEAN init)
    {
        m_Flags.HardwareInit = init;
    }
    BOOLEAN IsPointerEnabled() const
    {
        return m_Flags.PointerEnabled;
    }
    void SetPointerEnabled(BOOLEAN Enabled)
    {
        m_Flags.PointerEnabled = Enabled;
    }
    BOOLEAN IsVgaDevice(void) const
    {
        return m_Flags.VgaDevice;
    }
    void SetVgaDevice(BOOLEAN Vga)
    {
        m_Flags.VgaDevice = Vga;
    }
    BOOLEAN IsFlexResolution(void) const
    {
        return m_Flags.FlexResolution;
    }
    void SetFlexResolution(BOOLEAN FlexRes)
    {
        m_Flags.FlexResolution = FlexRes;
    }
    BOOLEAN IsUsePhysicalMemory() const
    {
        return m_Flags.UsePhysicalMemory;
    }
    void SetUsePhysicalMemory(BOOLEAN enable)
    {
        m_Flags.UsePhysicalMemory = enable;
    }
    BOOLEAN IsUsePresentProgress() const
    {
        return m_Flags.UsePresentProgress;
    }
    void SetUsePresentProgress(BOOLEAN enable)
    {
        m_Flags.UsePresentProgress = enable;
    }
    BOOLEAN IsRenderOnly() const
    {
        return m_Flags.RenderOnly;
    }
    void SetRenderOnly(BOOLEAN enable)
    {
        m_Flags.RenderOnly = enable;
    }
    void SetPersistentDispMode0Width(USHORT res)
    {
        m_PersistentDispMode0Width = res;
    }
    USHORT GetPersistentDispMode0Width()
    {
        return m_PersistentDispMode0Width;
    }
    void SetPersistentDispMode0Height(USHORT res)
    {
        m_PersistentDispMode0Height = res;
    }
    USHORT GetPersistentDispMode0Height()
    {
        return m_PersistentDispMode0Height;
    }
    BOOLEAN IsPersistentDispMode0Set()
    {
        return (m_PersistentDispMode0Width > 0) && (m_PersistentDispMode0Height > 0);
    }
#pragma code_seg(pop)

    NTSTATUS StartDevice(_In_ DXGK_START_INFO *pDxgkStartInfo,
                         _In_ DXGKRNL_INTERFACE *pDxgkInterface,
                         _Out_ ULONG *pNumberOfViews,
                         _Out_ ULONG *pNumberOfChildren);
    NTSTATUS StopDevice(VOID);
    VOID ResetDevice(VOID);
    NTSTATUS ResetFromTimeout(void);
    NTSTATUS RestartFromTimeout(void);
    NTSTATUS DispatchIoRequest(_In_ ULONG VidPnSourceId, _In_ VIDEO_REQUEST_PACKET *pVideoRequestPacket);
    NTSTATUS SetPowerState(_In_ ULONG HardwareUid,
                           _In_ DEVICE_POWER_STATE DevicePowerState,
                           _In_ POWER_ACTION ActionType);
    NTSTATUS QueryChildRelations(_Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR *pChildRelations,
                                 _In_ ULONG ChildRelationsSize);
    NTSTATUS QueryChildStatus(_Inout_ DXGK_CHILD_STATUS *pChildStatus, _In_ BOOLEAN NonDestructiveOnly);
    NTSTATUS QueryDeviceDescriptor(_In_ ULONG ChildUid, _Inout_ DXGK_DEVICE_DESCRIPTOR *pDeviceDescriptor);
    NTSTATUS GetScanLine(_Inout_ DXGKARG_GETSCANLINE *pGetScanLine);
    NTSTATUS ControlInterrupt(_In_ DXGK_INTERRUPT_TYPE interruptType, _In_ BOOLEAN enableInterrupt);
    BOOLEAN InterruptRoutine(_In_ ULONG MessageNumber);
    VOID DpcRoutine(VOID);
    NTSTATUS QueryAdapterInfo(_In_ CONST DXGKARG_QUERYADAPTERINFO *pQueryAdapterInfo);
    NTSTATUS SetPointerPosition(_In_ CONST DXGKARG_SETPOINTERPOSITION *pSetPointerPosition);
    NTSTATUS SetPointerShape(_In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape);
    NTSTATUS Escape(_In_ CONST DXGKARG_ESCAPE *pEscape);
    NTSTATUS PresentDisplayOnly(_In_ CONST DXGKARG_PRESENT_DISPLAYONLY *pPresentDisplayOnly);
    NTSTATUS QueryInterface(_In_ CONST PQUERY_INTERFACE QueryInterface);
    NTSTATUS IsSupportedVidPn(_Inout_ DXGKARG_ISSUPPORTEDVIDPN *pIsSupportedVidPn);
    NTSTATUS RecommendFunctionalVidPn(_In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN *CONST pRecommendFunctionalVidPn);
    NTSTATUS RecommendVidPnTopology(_In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY *CONST pRecommendVidPnTopology);
    NTSTATUS RecommendMonitorModes(_In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes);
    NTSTATUS EnumVidPnCofuncModality(_In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *CONST pEnumCofuncModality);
    NTSTATUS SetVidPnSourceVisibility(_In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY *pSetVidPnSourceVisibility);
    NTSTATUS CommitVidPn(_In_ CONST DXGKARG_COMMITVIDPN *CONST pCommitVidPn);
    NTSTATUS
    UpdateActiveVidPnPresentPath(_In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *CONST pUpdateActiveVidPnPresentPath);
    NTSTATUS QueryVidPnHWCapability(_Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY *pVidPnHWCaps);
    NTSTATUS StopDeviceAndReleasePostDisplayOwnership(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                      _Out_ DXGK_DISPLAY_INFORMATION *pDisplayInfo);
    NTSTATUS SystemDisplayEnable(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                 _In_ PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                                 _Out_ UINT *pWidth,
                                 _Out_ UINT *pHeight,
                                 _Out_ D3DDDIFORMAT *pColorFormat);
    VOID SystemDisplayWrite(_In_reads_bytes_(SourceHeight *SourceStride) VOID *pSource,
                            _In_ UINT SourceWidth,
                            _In_ UINT SourceHeight,
                            _In_ UINT SourceStride,
                            _In_ INT PositionX,
                            _In_ INT PositionY);
    NTSTATUS SetRegisterConfigInfo(void);

    PDXGKRNL_INTERFACE GetDxgkInterface(void)
    {
        return &m_DxgkInterface;
    }
#if defined(VIOGPU_NATIVE_CONTEXT)
    UINT Allocate2DResourceId(void);
    BOOLEAN Release2DResourceId(_In_ UINT resourceId);
    VIOGPU_HOST_CONTEXT_RESULT Create2DResourceBacking(_In_ UINT resourceId,
                                                       _In_ UINT format,
                                                       _In_ UINT width,
                                                       _In_ UINT height,
                                                       _In_ SIZE_T backingSize,
                                                       _In_reads_(entryCount) const GPU_MEM_ENTRY *entries,
                                                       _In_ UINT entryCount,
                                                       _Inout_ VIOGPU_2D_RESOURCE_STATE *resourceState,
                                                       _Inout_ ULONGLONG *resourceResetGeneration);
    VIOGPU_HOST_CONTEXT_RESULT Destroy2DResource(_In_ UINT resourceId,
                                                 _Inout_ VIOGPU_2D_RESOURCE_STATE *resourceState,
                                                 _Inout_ ULONGLONG *resourceResetGeneration,
                                                 _Out_ BOOLEAN *released);
    BOOLEAN Reconcile2DResourceAfterReset(_Inout_ VIOGPU_2D_RESOURCE_STATE *resourceState,
                                          _Inout_ ULONGLONG *resourceResetGeneration,
                                          _Out_ BOOLEAN *retired);
    VIOGPU_HOST_CONTEXT_RESULT Present2DResource(_In_ UINT resourceId,
                                                 _In_ ULONGLONG offset,
                                                 _In_ UINT width,
                                                 _In_ UINT height,
                                                 _In_ UINT x,
                                                 _In_ UINT y,
                                                 _Inout_ VIOGPU_2D_RESOURCE_STATE *resourceState,
                                                 _Inout_ ULONGLONG *resourceResetGeneration);
    VIOGPU_HOST_CONTEXT_RESULT Set2DScanout(_In_ UINT scanoutId,
                                            _In_ UINT resourceId,
                                            _In_ UINT width,
                                            _In_ UINT height,
                                            _Out_ UINT *previousResourceId);
    VIOGPU_HOST_CONTEXT_RESULT Detach2DScanoutResource(_In_ UINT resourceId, _Out_ BOOLEAN *detached);
    BOOLEAN Query2DScanoutResource(_In_ UINT resourceId, _Out_ BOOLEAN *active);
    PGPU_VBUFFER PrepareNativeSubmit(_In_ UINT contextId, _In_ const void *command, _In_ UINT commandSize);
    BOOLEAN RefreshNativeSubmit(_In_ PGPU_VBUFFER buffer, _In_ const void *command, _In_ UINT commandSize);
    int QueueNativeSubmit(_In_ PGPU_VBUFFER buffer, _In_ ULONGLONG fenceId);
    BOOLEAN ReleaseNativeSubmitBuffer(_In_ PGPU_VBUFFER buffer);
    BOOLEAN IsNativeContextGenerationCurrent(_In_ LONG generation, _In_ ULONGLONG resetGeneration) const;
    BOOLEAN IsNativeContextResetRetired(_In_ ULONGLONG resetGeneration) const;
    UINT AllocateNativeResourceId(_In_ ULONGLONG expectedResetGeneration);
    BOOLEAN AcquireNativeContextSnapshotForAllocation(_In_ ULONGLONG requestedIova,
                                                      _In_ SIZE_T backingSize,
                                                      _In_ ULONGLONG expectedResetGeneration,
                                                      _In_ UINT expectedContextId,
                                                      _Out_ VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot) const;
#endif
    _IRQL_requires_max_(DISPATCH_LEVEL) BOOLEAN QueryNativeContextReadiness(_Out_ PGPU_CAPSET_DRM capset,
                                                                            _Out_opt_ UINT *capsetVersion,
                                                                            _Out_opt_ UINT *capsetSize,
                                                                            _Out_opt_ ULONGLONG *resetGeneration);
    NTSTATUS CreateNativeContext(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context,
                                 _In_ ULONGLONG expectedResetGeneration);
    NTSTATUS DestroyNativeContext(_Inout_ VIOGPU_NATIVE_CONTEXT_REGISTRATION *context, _Out_ BOOLEAN *released);
    BOOLEAN IsHardwareResetRequested(void) const
    {
        return InterlockedCompareExchange(&m_HardwareResetState, VioGpuHardwareActive, VioGpuHardwareActive) !=
               VioGpuHardwareActive;
    }
    VIOGPU_HARDWARE_RESET_STATE QueryHardwareResetState(void) const
    {
        return static_cast<VIOGPU_HARDWARE_RESET_STATE>(InterlockedCompareExchange(&m_HardwareResetState,
                                                                                   VioGpuHardwareActive,
                                                                                   VioGpuHardwareActive));
    }
    __declspec(noinline) VOID RequestHardwareResetAtAnyIrql(void);
    BOOLEAN IsHardwareInterruptDispatchAllowed(void) const
    {
        LONG state = InterlockedCompareExchange(&m_HardwareResetState, VioGpuHardwareActive, VioGpuHardwareActive);
        return state == VioGpuHardwareActive || state == VioGpuHardwareRecovering;
    }
#if defined(VIOGPU_NATIVE_CONTEXT)
    BOOLEAN AcquireNativeSubmissionOperation(void) const;
    void ReleaseNativeSubmissionOperation(void) const;
    void NotifyNativeSubmissionCompletion(_In_ UINT fenceId,
                                          _In_ UINT nodeOrdinal,
                                          _In_ UINT engineOrdinal,
                                          _In_ BOOLEAN queueDpc);
    __declspec(noinline) void NotifyNativeSubmissionFault(_In_ UINT fenceId,
                                                          _In_ NTSTATUS status,
                                                          _In_ UINT nodeOrdinal,
                                                          _In_ UINT engineOrdinal,
                                                          _In_ BOOLEAN queueDpc,
                                                          _In_ DWORD presentSubmitStage = 0,
                                                          _In_ NTSTATUS presentSubmitStatus = STATUS_SUCCESS,
                                                          _In_ DWORD presentSubmitDetail = 0);
    void NotifyNativeSoftwareCompletion(_In_ UINT fenceId, _In_ UINT nodeOrdinal, _In_ UINT engineOrdinal);
    BOOLEAN QueueNativeSoftwareSubmissionCompletion(_In_ UINT fenceId, _In_ UINT nodeOrdinal, _In_ UINT engineOrdinal);
    BOOLEAN DrainNativeSoftwareSubmissionCompletionsFromDpc(void);
    BOOLEAN CompleteNativeSystemSubmission(_In_ UINT fenceId, _In_ UINT nodeOrdinal, _In_ UINT engineOrdinal);
    BOOLEAN NotifyNativeCompletedFence(_In_ UINT completedFence,
                                       _In_ UINT nodeOrdinal,
                                       _In_ UINT engineOrdinal,
                                       _In_ BOOLEAN queueDpc);
    BOOLEAN NotifyNativeSchedulerInterrupt(_In_ const DXGKARGCB_NOTIFY_INTERRUPT_DATA *notification,
                                           _In_ BOOLEAN queueDpc);
    BOOLEAN RecordNativeSubmissionFence(_In_ UINT fenceId);
    BOOLEAN RetireNativeSubmissionFence(_In_ UINT fenceId, _Out_ UINT *completedFence);
    BOOLEAN IsNativeFenceQueueEmpty(void);
    void ResetNativeFenceTracker(void);
    void InvalidateNativeFenceTracker(void);
    void CompleteNativeFenceReset(void);
    BOOLEAN QueueNativePassiveWork(_Inout_ VIOGPU_NATIVE_PASSIVE_WORK *work, _In_ UINT fenceId);
    VOID CompleteNativePassiveWork(_Inout_ VIOGPU_NATIVE_PASSIVE_WORK *work);
    VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIP CancelNativePassiveWork(_Inout_ VIOGPU_NATIVE_PASSIVE_WORK *work);
    VOID CloseNativePassiveQueue(void);
    BOOLEAN WaitForNativePassiveQueueIdle(void);
    BOOLEAN OpenNativePassiveQueue(void);
    VOID RequestWddmSubmissionDrainAtAnyIrql(void);
    BOOLEAN WaitForWddmSubmissionDrain(void);
    VOID CloseWddmPresentTransactions(void);
    BOOLEAN OpenWddmPresentTransactions(void);
    BOOLEAN RegisterWddmPresentTransaction(_Inout_ LIST_ENTRY *link);
    BOOLEAN UnregisterWddmPresentTransaction(_Inout_ LIST_ENTRY *link);
    PLIST_ENTRY PopWddmPresentTransactionForReset(void);
    UINT QueryNativeCompletedFence(void) const
    {
        return static_cast<UINT>(InterlockedCompareExchange(const_cast<volatile LONG *>(&m_NativeCompletedFence),
                                                            0,
                                                            0));
    }
    UINT QueryNativeSubmittedFence(void) const
    {
        return static_cast<UINT>(InterlockedCompareExchange(const_cast<volatile LONG *>(&m_NativeSubmittedFence),
                                                            0,
                                                            0));
    }
    VOID RecordNativeStartDiagnostic(_In_ VIOGPU_NATIVE_START_STAGE stage, _In_ NTSTATUS status, _In_ DWORD detail);
    VOID RecordNativeContextCreateDiagnostic(_In_ VIOGPU_NATIVE_CONTEXT_CREATE_STAGE stage,
                                             _In_ NTSTATUS status,
                                             _In_ DWORD detail);
    VOID RecordNativeContextDestroyDiagnostic(_In_ VIOGPU_NATIVE_CONTEXT_DESTROY_STAGE stage,
                                              _In_ NTSTATUS status,
                                              _In_ DWORD detail,
                                              _In_ DWORD hostResult,
                                              _In_ UINT contextId,
                                              _In_ DWORD contextState,
                                              _In_ DWORD ownerState,
                                              _In_ BOOLEAN released,
                                              _In_ BOOLEAN retrying,
                                              _In_ BOOLEAN ownerRetained);
    VOID RecordNativeAllocationRangeDiagnostic(_In_ NTSTATUS status,
                                               _In_ DWORD reason,
                                               _In_ DWORD rangeCount,
                                               _In_ ULONGLONG requestedIova,
                                               _In_ ULONGLONG collisionIova,
                                               _In_ DWORD collisionLength,
                                               _In_ UINT collisionResourceId,
                                               _In_ UINT collisionContextId,
                                               _In_ DWORD registrationState,
                                               _In_ DWORD registrationReferences,
                                               _In_ BOOLEAN allocationDestroying,
                                               _In_ DWORD allocationHostState);
    VOID RecordNativeAllocationDestroyDiagnostic(_In_ DWORD stage,
                                                  _In_ NTSTATUS status,
                                                  _In_ DWORD detail,
                                                  _In_ BOOLEAN nativeContextPresent,
                                                  _In_ BOOLEAN contextRangePresent,
                                                  _In_ BOOLEAN contextRangeLinked,
                                                  _In_ DWORD registrationState,
                                                  _In_ DWORD registrationReferences,
                                                  _In_ BOOLEAN allocationDestroying,
                                                  _In_ DWORD allocationHostState,
                                                  _In_ UINT contextId,
                                                  _In_ UINT resourceId,
                                                  _In_ DWORD rangeCount,
                                                  _In_ ULONGLONG requestedIova,
                                                  _In_ ULONGLONG rangeIova,
                                                  _In_ SIZE_T rangeLength);
    VOID RecordNativeContextCreateResponseDiagnostic(_In_ const VIOGPU_HOST_CONTEXT_RESPONSE_DIAGNOSTIC *diagnostic);
    VOID RecordNativeContextMapResponseDiagnostic(_In_ const VIOGPU_NATIVE_MAP_RESPONSE_DIAGNOSTIC *diagnostic);
    VOID RecordNativeContextParameterDiagnostic(_Inout_ PVIOGPU_NATIVE_CONTEXT_PARAMETER_DIAGNOSTIC diagnostic);
    VOID RecordNativeContextMapMemoryDiagnostic(_In_ NTSTATUS status,
                                                _In_ ULONGLONG physicalAddress,
                                                _In_ ULONGLONG length,
                                                _In_ UINT bar,
                                                _In_ ULONGLONG regionOffset,
                                                _In_ BOOLEAN attempted,
                                                _In_ BOOLEAN mapped);
    VOID RecordNativeQueryAdapterInfoDiagnostic(_In_ UINT type,
                                                _In_ NTSTATUS status,
                                                _In_ UINT inputDataSize,
                                                _In_ UINT outputDataSize);
    VOID RecordNativePresentDiagnostic(_In_ DWORD reason,
                                       _In_ NTSTATUS status,
                                       _In_ const VIOGPU_NATIVE_PRESENT_DIAGNOSTIC *diagnostic);
    BOOLEAN ClaimNativePresentExecutionDiagnostic(void);
    VOID RecordNativePresentExecutionDiagnostic(_In_ const VIOGPU_NATIVE_PRESENT_EXECUTION_DIAGNOSTIC *diagnostic);
    VOID RecordNativePresentExecutionResetProvenance(void);
    VOID RecordNativePresentCopyProbe(_In_ const VIOGPU_NATIVE_PRESENT_COPY_PROBE *probe);
#endif

  private:
#if defined(VIOGPU_NATIVE_CONTEXT)
    static VOID NativePassiveWorker(_In_opt_ PVOID context);
    VOID RunNativePassiveWorker(void);
    static VOID WddmSubmissionDrainWorker(_In_opt_ PVOID context);
    VOID QueueWddmSubmissionDrainWorker(void);
    VOID RunWddmSubmissionDrainWorker(void);
#endif
    BOOLEAN CheckHardware();
    NTSTATUS UnwindFailedStart(_In_ NTSTATUS failureStatus);
    NTSTATUS WriteRegistryString(_In_ HANDLE DevInstRegKeyHandle, _In_ PCWSTR pszwValueName, _In_ PCSTR pszValue);
    NTSTATUS WriteRegistryDWORD(_In_ HANDLE DevInstRegKeyHandle, _In_ PCWSTR pszwValueName, _In_ PDWORD pdwValue);
    NTSTATUS ReadRegistryDWORD(_In_ HANDLE DevInstRegKeyHandle, _In_ PCWSTR pszwValueName, _Inout_ PDWORD pdwValue);
    NTSTATUS SetSourceModeAndPath(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode,
                                  CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath);
    NTSTATUS AddSingleMonitorMode(_In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes);
    NTSTATUS AddSingleSourceMode(_In_ CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface,
                                 D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet,
                                 D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId);
    NTSTATUS AddSingleTargetMode(_In_ CONST DXGK_VIDPNTARGETMODESET_INTERFACE *pVidPnTargetModeSetInterface,
                                 D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet,
                                 _In_opt_ CONST D3DKMDT_VIDPN_SOURCE_MODE *pVidPnPinnedSourceModeInfo,
                                 D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId);
    NTSTATUS IsVidPnSourceModeFieldsValid(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode) const;
    NTSTATUS IsVidPnPathFieldsValid(CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath) const;
    NTSTATUS SetRegisterInfo(_In_ ULONG Id, _In_ DWORD MemSize);
    NTSTATUS GetRegisterInfo(void);
    VOID BuildVideoSignalInfo(D3DKMDT_VIDEO_SIGNAL_INFO *pVideoSignalInfo, PVIDEO_MODE_INFORMATION pModeInfo);
};
