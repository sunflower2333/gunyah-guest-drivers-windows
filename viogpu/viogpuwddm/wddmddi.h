#pragma once

#include "../viogpudo/driver.h"
#include "../viogpudo/viogpudo.h"
#include "../shared/viogpu_wddm_abi.h"

// This target is compile-only until VirtIO fence completion and TDR recovery
// are connected to the WDDM scheduler callbacks.
#if !defined(VIOGPU_WDDM_CI_ONLY)
#error viogpuwddm is not installable yet; build it only with VIOGPU_WDDM_CI_ONLY
#endif

class VioGpuDod;

VOID VioGpuWddmBuildInitializationData(_Out_ DRIVER_INITIALIZATION_DATA *initialData);

struct VIOGPU_WDDM_KMD_DMA_PRIVATE
{
    ULONG Signature;
    USHORT Version;
    USHORT Kind;
    PVOID DmaBuffer;
    UINT DmaBufferSize;
    UINT CommandLength;
    UINT ContextId;
    LONG Generation;
    ULONGLONG ResetGeneration;
    UINT Flags;
    PVOID Packet;
    UINT PacketLength;
    UINT Reserved;
    PVOID Submission;
};

enum : USHORT
{
    VioGpuWddmDmaPrivateVersion = 1,
    VioGpuWddmDmaKindRender = 1,
    VioGpuWddmDmaKindPaging = 2,
};

enum : UINT
{
    VioGpuWddmSubmissionAllocationLimit = 128,
    VioGpuWddmPagingFlagPageIn = 1U << 0,
    VioGpuWddmPagingFlagPageOut = 1U << 1,
    VioGpuWddmPagingFlagFill = 1U << 2,
    VioGpuWddmPagingFlagDiscard = 1U << 3,
    VioGpuWddmPagingFlagTransferStart = 1U << 4,
    VioGpuWddmPagingFlagTransferEnd = 1U << 5,
    VioGpuWddmPagingFlagAllocationIdle = 1U << 6,
    VioGpuWddmPagingFlagSoftwareCompleted = 1U << 7,
};

struct VIOGPU_WDDM_PAGING_DMA_PACKET
{
    ULONG Signature;
    USHORT Version;
    USHORT Size;
    UINT Operation;
    UINT Flags;
    UINT ResourceId;
    UINT ContextId;
    LONG ContextGeneration;
    UINT Reserved;
    ULONGLONG ResetGeneration;
    ULONGLONG PlacementOffset;
    ULONGLONG PoolGeneration;
    ULONGLONG TransferOffset;
    ULONGLONG TransferSize;
};

static_assert(sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) == 72, "unexpected WDDM DMA private size");
static_assert(sizeof(VIOGPU_WDDM_PAGING_DMA_PACKET) == 72, "unexpected WDDM paging packet size");

struct VIOGPU_WDDM_RESOURCE
{
    ULONG Signature;
    VioGpuDod *Adapter;
    volatile LONG AllocationCount;
};

struct VIOGPU_WDDM_ALLOCATION_RANGE
{
    LIST_ENTRY Link;
    VIOGPU_NATIVE_CONTEXT_REGISTRATION *Registration;
    ULONGLONG Iova;
    SIZE_T Length;
    BOOLEAN Linked;
};

struct VIOGPU_WDDM_PAGING_RANGE
{
    LIST_ENTRY Link;
    SIZE_T Offset;
    SIZE_T Length;
};

enum VIOGPU_WDDM_ALLOCATION_HOST_STATE : LONG
{
    VioGpuWddmAllocationHostNone = 0,
    VioGpuWddmAllocationHostLive,
    VioGpuWddmAllocationHostUnknown,
};

enum VIOGPU_WDDM_ALLOCATION_PAGING_STATE : LONG
{
    VioGpuWddmAllocationPagingIdle = 0,
    VioGpuWddmAllocationPagingIn,
    VioGpuWddmAllocationPagingOut,
};

struct VIOGPU_WDDM_ALLOCATION
{
    ULONG Signature;
    KMUTEX LifecycleMutex;
    KSPIN_LOCK SubmissionLock;
    volatile LONG SubmissionReferences;
    volatile LONG OpenReferences;
    BOOLEAN Destroying;
    VioGpuDod *Adapter;
    VIOGPU_WDDM_RESOURCE *Resource;
    VIOGPU_NATIVE_CONTEXT_REGISTRATION *NativeContext;
    LONG ContextGeneration;
    ULONGLONG ContextResetGeneration;
    UINT ContextId;
    VIOGPU_WDDM_ALLOCATION_INFO PrivateData;
    SIZE_T BackingSize;
    VIOGPU_WDDM_ALLOCATION_RANGE *ContextRange;
    UINT ResourceId;
    UINT BlobId;
    ULONGLONG PlacementOffset;
    ULONGLONG PoolGeneration;
    VIOGPU_WDDM_ALLOCATION_HOST_STATE HostState;
    LONG BoundGeneration;
    ULONGLONG BoundResetGeneration;
    UINT BoundContextId;
    BOOLEAN PlacementValid;
    VIOGPU_WDDM_ALLOCATION_PAGING_STATE PagingState;
    ULONGLONG PagingPlacementOffset;
    ULONGLONG PagingPoolGeneration;
    LIST_ENTRY PagingRanges;
    SIZE_T PagingCoveredBytes;
    UINT Pitch;
    UINT Width;
    UINT Height;
    D3DDDIFORMAT Format;
    UINT Flags;
    UINT RefreshRateNumerator;
    UINT RefreshRateDenominator;
};

struct VIOGPU_WDDM_DEVICE
{
    ULONG Signature;
    VioGpuDod *Adapter;
    HANDLE RuntimeDevice;
    volatile LONG ReferenceState;
};

struct VIOGPU_WDDM_CONTEXT
{
    ULONG Signature;
    EX_RUNDOWN_REF Operations;
    BOOLEAN OperationsRundownCompleted;
    KSPIN_LOCK SubmissionLock;
    volatile LONG SubmissionReferences;
    BOOLEAN SubmissionClosing;
    LIST_ENTRY PendingSubmissions;
    VIOGPU_WDDM_DEVICE *Device;
    HANDLE RuntimeContext;
    UINT NodeOrdinal;
    UINT EngineAffinity;
    VIOGPU_NATIVE_CONTEXT_REGISTRATION NativeContext;
};

struct VIOGPU_WDDM_SUBMISSION_REFERENCE
{
    VIOGPU_WDDM_ALLOCATION *Allocation;
    UINT AllocationIndex;
    UINT Flags;
    ULONGLONG AllocationOffset;
    ULONGLONG Length;
    UINT PatchOffset;
    UINT Reserved;
};

enum VIOGPU_WDDM_SUBMISSION_STATE : LONG
{
    VioGpuWddmSubmissionPrepared = 0,
    VioGpuWddmSubmissionQueued,
    VioGpuWddmSubmissionQuarantined,
};

struct VIOGPU_WDDM_SUBMISSION
{
    ULONG Signature;
    LIST_ENTRY ContextLink;
    VIOGPU_WDDM_CONTEXT *Context;
    PVOID DmaBuffer;
    UINT DmaBufferSize;
    PVOID DmaPrivateData;
    UINT DmaPrivateDataSize;
    UINT CommandLength;
    UINT ContextId;
    LONG Generation;
    ULONGLONG ResetGeneration;
    ULONGLONG FenceId;
    PGPU_VBUFFER VirtioBuffer;
    VioGpuDod *Adapter;
    PVOID CommandStream;
    UINT CommandStreamSize;
    UINT CommandStreamOffset;
    BOOLEAN PatchApplied;
    volatile LONG State;
    UINT AllocationCount;
    VIOGPU_WDDM_ALLOCATION *Allocations[VioGpuWddmSubmissionAllocationLimit];
    VIOGPU_WDDM_SUBMISSION_REFERENCE References[VioGpuWddmSubmissionAllocationLimit];
};

struct VIOGPU_WDDM_OPEN_ALLOCATION
{
    ULONG Signature;
    VIOGPU_WDDM_ALLOCATION *Allocation;
    VIOGPU_WDDM_DEVICE *Device;
    BOOLEAN ReadOnly;
};

DXGKDDI_QUERYADAPTERINFO VioGpuWddmQueryAdapterInfo;
DXGKDDI_CREATEALLOCATION VioGpuWddmCreateAllocation;
DXGKDDI_DESTROYALLOCATION VioGpuWddmDestroyAllocation;
DXGKDDI_DESCRIBEALLOCATION VioGpuWddmDescribeAllocation;
DXGKDDI_GETSTANDARDALLOCATIONDRIVERDATA VioGpuWddmGetStandardAllocationDriverData;
DXGKDDI_OPENALLOCATIONINFO VioGpuWddmOpenAllocation;
DXGKDDI_CLOSEALLOCATION VioGpuWddmCloseAllocation;
DXGKDDI_CREATEDEVICE VioGpuWddmCreateDevice;
DXGKDDI_DESTROYDEVICE VioGpuWddmDestroyDevice;
DXGKDDI_CREATECONTEXT VioGpuWddmCreateContext;
DXGKDDI_DESTROYCONTEXT VioGpuWddmDestroyContext;
DXGKDDI_ESCAPE VioGpuWddmEscape;
DXGKDDI_BUILDPAGINGBUFFER VioGpuWddmBuildPagingBuffer;
DXGKDDI_RENDER VioGpuWddmRender;
DXGKDDI_PRESENT VioGpuWddmPresent;
DXGKDDI_PATCH VioGpuWddmPatch;
DXGKDDI_SUBMITCOMMAND VioGpuWddmSubmitCommand;
DXGKDDI_PREEMPTCOMMAND VioGpuWddmPreemptCommand;
DXGKDDI_QUERYCURRENTFENCE VioGpuWddmQueryCurrentFence;
DXGKDDI_RESETFROMTIMEOUT VioGpuWddmResetFromTimeout;
DXGKDDI_RESTARTFROMTIMEOUT VioGpuWddmRestartFromTimeout;
DXGKDDI_SETVIDPNSOURCEADDRESS VioGpuWddmSetVidPnSourceAddress;
