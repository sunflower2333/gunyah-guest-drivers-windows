#pragma once

#include "../viogpudo/driver.h"
#include "../viogpudo/viogpudo.h"
#include "../shared/viogpu_wddm_abi.h"

// Select the crosvm Native Context implementation when this header is used by
// the full graphics miniport. The display-only target leaves it undefined.
#if !defined(VIOGPU_NATIVE_CONTEXT)
#error viogpuwddm requires VIOGPU_NATIVE_CONTEXT
#endif

class VioGpuDod;

VOID VioGpuWddmBuildInitializationData(_Out_ DRIVER_INITIALIZATION_DATA *initialData);
VOID VioGpuWddmDrainPresentTransactions(_In_ VioGpuDod *adapter);

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
    VioGpuWddmDebugSnapshotVersion = 1,
    VioGpuWddmDmaKindRender = 1,
    VioGpuWddmDmaKindPaging = 2,
    VioGpuWddmDmaKindPresent = 3,
};

enum : UINT
{
    /* Legacy MSM submits must carry every live BO because shader-visible raw
     * IOVAs do not provide a complete resource dependency graph.  This value
     * matches the UMD and still fits, with 256 command records, in the 64 KiB
     * Native Context DMA buffer. */
    VioGpuWddmSubmissionAllocationLimit = 1024,
    /* Keep the context-scoped UMD fence queue bounded at the same order of
     * magnitude as the Host scheduler tracker.  A full queue fails closed
     * instead of allowing an untracked completion endpoint. */
    VioGpuWddmContextFenceTrackerCapacity = 4096,
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
    ULONGLONG TransferOffset;
    ULONGLONG TransferSize;
};

struct VIOGPU_WDDM_PRESENT_DMA_PACKET
{
    ULONG Signature;
    USHORT Version;
    USHORT Size;
    UINT Flags;
    UINT SourceResourceId;
    UINT DestinationResourceId;
    UINT RectCount;
    UINT Reserved;
    ULONGLONG SourcePlacementOffset;
    ULONGLONG DestinationPlacementOffset;
    ULONGLONG DestinationResetGeneration;
};

static_assert(sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE) == 72, "unexpected WDDM DMA private size");
static_assert(sizeof(VIOGPU_WDDM_PAGING_DMA_PACKET) == 64, "unexpected WDDM paging packet size");
static_assert(sizeof(VIOGPU_WDDM_PRESENT_DMA_PACKET) == 56, "unexpected WDDM present packet size");

struct VIOGPU_WDDM_DEBUG_SNAPSHOT
{
    ULONG Signature;
    USHORT Version;
    USHORT Size;
    UINT Reason;
    LONG HardwareResetState;
    UINT SubmittedFence;
    UINT CompletedFence;
    UINT CurrentIrql;
    UINT Reserved;
};

static_assert(sizeof(VIOGPU_WDDM_DEBUG_SNAPSHOT) == 32, "unexpected WDDM debug snapshot size");

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

enum VIOGPU_WDDM_ALLOCATION_HOST_STATE : LONG
{
    VioGpuWddmAllocationHostNone = 0,
    VioGpuWddmAllocationHostLive,
    VioGpuWddmAllocationHostUnknown,
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
    VIOGPU_2D_RESOURCE_STATE Resource2DState;
    ULONGLONG Resource2DResetGeneration;
    ULONGLONG PlacementOffset;
    PPFN_NUMBER AperturePfns;
    PUCHAR ApertureMappedPages;
    SIZE_T AperturePageCount;
    SIZE_T ApertureMappedPageCount;
    SIZE_T ApertureBasePage;
    PMDL ApertureMdl;
    PVOID ApertureAddress;
    VIOGPU_WDDM_ALLOCATION_HOST_STATE HostState;
    LONG BoundGeneration;
    ULONGLONG BoundResetGeneration;
    UINT BoundContextId;
    BOOLEAN PlacementValid;
    BOOLEAN ApertureBaseValid;
    UINT Pitch;
    UINT Width;
    UINT Height;
    D3DDDIFORMAT Format;
    UINT Flags;
    UINT RefreshRateNumerator;
    UINT RefreshRateDenominator;
};

enum VIOGPU_WDDM_PAGING_TRANSACTION_STATE : LONG
{
    VioGpuWddmPagingTransactionAny = -1,
    VioGpuWddmPagingTransactionInvalid = 0,
    VioGpuWddmPagingTransactionBuilt,
    VioGpuWddmPagingTransactionQueued,
    VioGpuWddmPagingTransactionExecuting,
    VioGpuWddmPagingTransactionFinished,
    VioGpuWddmPagingTransactionCancelled,
};

struct VIOGPU_WDDM_PAGING_TRANSACTION
{
    ULONG Signature;
    volatile LONG State;
    volatile LONG ReferenceHeld;
    volatile LONG ExecutionStarted;
    volatile LONG CancelRequested;
    VioGpuDod *Adapter;
    VIOGPU_WDDM_ALLOCATION *Allocation;
    UINT FillPattern;
    UINT Operation;
    UINT Flags;
    SIZE_T TransferOffset;
    SIZE_T TransferSize;
    ULONGLONG PlacementOffset;
    UINT ResourceId;
    UINT ContextId;
    LONG ContextGeneration;
    ULONGLONG ResetGeneration;
    BOOLEAN TransferDataComplete;
};

struct VIOGPU_WDDM_PAGING_PRIVATE
{
    VIOGPU_WDDM_KMD_DMA_PRIVATE Header;
    VIOGPU_WDDM_PAGING_TRANSACTION Transaction;
    VIOGPU_NATIVE_PASSIVE_WORK Work;
    PVOID BatchPrivateData;
    UINT BatchPrivateDataSize;
    UINT BatchPrivateStart;
    UINT BatchPrivateEnd;
    UINT BatchFenceId;
    UINT BatchNodeOrdinal;
    UINT BatchEngineOrdinal;
};

static_assert(FIELD_OFFSET(VIOGPU_WDDM_PAGING_PRIVATE, Header) == 0,
              "paging header must remain the private record prefix");
static_assert(sizeof(VIOGPU_WDDM_PAGING_PRIVATE) <= PAGE_SIZE, "paging private record must fit one paging buffer");

enum VIOGPU_WDDM_CONTEXT_FENCE_STATE : LONG
{
    VioGpuWddmContextFenceFree = 0,
    VioGpuWddmContextFencePending,
    VioGpuWddmContextFenceRetired,
};

struct VIOGPU_WDDM_CONTEXT_FENCE_ENTRY
{
    UINT FenceId;
    VIOGPU_WDDM_CONTEXT_FENCE_STATE State;
};

static_assert(sizeof(VIOGPU_WDDM_CONTEXT_FENCE_ENTRY) == 8, "unexpected context fence entry size");

struct VIOGPU_WDDM_DEVICE
{
    ULONG Signature;
    VioGpuDod *Adapter;
    HANDLE RuntimeDevice;
    volatile LONG ReferenceState;
};

enum VIOGPU_WDDM_CONTEXT_TYPE : LONG
{
    VioGpuWddmContextNative = 0,
    VioGpuWddmContextSystem,
    VioGpuWddmContextGdi,
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
    UINT UmdFenceHead;
    UINT UmdFenceCount;
    VIOGPU_WDDM_CONTEXT_FENCE_ENTRY UmdFences[VioGpuWddmContextFenceTrackerCapacity];
    volatile LONG SubmittedUmdFence;
    volatile LONG CompletedUmdFence;
    VIOGPU_WDDM_DEVICE *Device;
    HANDLE RuntimeContext;
    VIOGPU_WDDM_CONTEXT_TYPE Type;
    UINT NodeOrdinal;
    UINT EngineAffinity;
    VIOGPU_NATIVE_CONTEXT_REGISTRATION NativeContext;
};

enum VIOGPU_WDDM_CONTEXT_SUBMISSION_KIND : ULONG
{
    VioGpuWddmContextSubmissionRender = 1,
    VioGpuWddmContextSubmissionPresent = 2,
};

struct VIOGPU_WDDM_CONTEXT_SUBMISSION_ENTRY
{
    LIST_ENTRY Link;
    VIOGPU_WDDM_CONTEXT_SUBMISSION_KIND Kind;
    PVOID Owner;
    VIOGPU_WDDM_CONTEXT *Context;
};

enum VIOGPU_WDDM_PRESENT_STATE : LONG
{
    VioGpuWddmPresentInvalid = 0,
    VioGpuWddmPresentBuilt,
    VioGpuWddmPresentPatched,
    VioGpuWddmPresentQueued,
    VioGpuWddmPresentExecuting,
    VioGpuWddmPresentFinished,
    VioGpuWddmPresentCancelled,
};

enum VIOGPU_WDDM_PRESENT_DIAGNOSTIC_REASON : DWORD
{
    VioGpuWddmPresentDiagnosticNone = 0,
    VioGpuWddmPresentDiagnosticNativeSourceIdentity = 1,
    VioGpuWddmPresentDiagnosticGdiSourcePlacement = 2,
    VioGpuWddmPresentDiagnosticGdiSourceIdentity = 3,
    VioGpuWddmPresentDiagnosticSourceObject = 4,
    VioGpuWddmPresentDiagnosticDestinationObject = 5,
    VioGpuWddmPresentDiagnosticSourcePlacement = 6,
    VioGpuWddmPresentDiagnosticDestinationBacking = 7,
    VioGpuWddmPresentDiagnosticDestinationPlacement = 8,
    VioGpuWddmPresentDiagnosticGeometry = 9,
    VioGpuWddmPresentDiagnosticSourcePrepatch = 10,
    VioGpuWddmPresentDiagnosticDestinationPrepatch = 11,
    VioGpuWddmPresentDiagnosticContextReference = 12,
    VioGpuWddmPresentDiagnosticSourceReference = 13,
    VioGpuWddmPresentDiagnosticDestinationReference = 14,
    VioGpuWddmPresentDiagnosticSourceLifecycle = 15,
    VioGpuWddmPresentDiagnosticDestinationLifecycle = 16,
    VioGpuWddmPresentDiagnosticTransactionReference = 17,
    VioGpuWddmPresentDiagnosticTransactionRegistration = 18,
    VioGpuWddmPresentDiagnosticContextPublication = 19,
};

enum VIOGPU_WDDM_PRESENT_EXECUTION_STAGE : DWORD
{
    VioGpuWddmPresentExecuteNone = 0,
    VioGpuWddmPresentExecuteInvalidTransaction = 1,
    VioGpuWddmPresentExecuteSourceLifecycle = 2,
    VioGpuWddmPresentExecuteDestinationLifecycle = 3,
    VioGpuWddmPresentExecuteGdiSourceReconcile = 4,
    VioGpuWddmPresentExecuteSourceIdentity = 5,
    VioGpuWddmPresentExecuteSourceObject = 6,
    VioGpuWddmPresentExecuteDestinationObject = 7,
    VioGpuWddmPresentExecuteAliasedAllocations = 8,
    VioGpuWddmPresentExecuteDestinationPrimary = 9,
    VioGpuWddmPresentExecuteSourcePlacement = 10,
    VioGpuWddmPresentExecuteDestinationBacking = 11,
    VioGpuWddmPresentExecuteDestinationPlacement = 12,
    VioGpuWddmPresentExecuteGeometry = 13,
    VioGpuWddmPresentExecuteSourcePlacementOffset = 14,
    VioGpuWddmPresentExecuteDestinationPlacementOffset = 15,
    VioGpuWddmPresentExecuteDestinationResetGeneration = 16,
    VioGpuWddmPresentExecuteCopyAddress = 17,
    VioGpuWddmPresentExecuteCancelled = 18,
    VioGpuWddmPresentExecuteHostPresent = 19,
    VioGpuWddmPresentExecuteSubmissionOperation = 20,
    VioGpuWddmPresentExecuteTransactionRetire = 21,
    VioGpuWddmPresentExecuteComplete = 0x0FFF,
};

struct VIOGPU_WDDM_PRESENT_TRANSACTION
{
    ULONG Signature;
    volatile LONG ReferenceCount;
    volatile LONG State;
    volatile LONG CancelRequested;
    volatile LONG WorkReferenceHeld;
    VIOGPU_WDDM_CONTEXT_SUBMISSION_ENTRY ContextEntry;
    LIST_ENTRY AdapterLink;
    VIOGPU_NATIVE_PASSIVE_WORK Work;
    VIOGPU_WDDM_CONTEXT *Context;
    VioGpuDod *Adapter;
    VIOGPU_WDDM_ALLOCATION *Source;
    VIOGPU_WDDM_ALLOCATION *Destination;
    PVOID DmaBuffer;
    UINT DmaBufferSize;
    VIOGPU_WDDM_KMD_DMA_PRIVATE *PrivateData;
    UINT PrivateDataSize;
    UINT SourceAllocationIndex;
    UINT DestinationAllocationIndex;
    RECT SourceRect;
    RECT DestinationRect;
    RECT *DestinationSubRects;
    UINT RectCount;
    ULONGLONG SourcePlacementOffset;
    ULONGLONG DestinationPlacementOffset;
    ULONGLONG DestinationResetGeneration;
    UINT FenceId;
    BOOLEAN FullyPrepatched;
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
    VioGpuWddmSubmissionPatching,
    VioGpuWddmSubmissionPatched,
    VioGpuWddmSubmissionSubmitClaimed,
    VioGpuWddmSubmissionEngineQueued,
    VioGpuWddmSubmissionHostIssued,
    VioGpuWddmSubmissionQuarantined,
};

struct VIOGPU_WDDM_SUBMISSION
{
    ULONG Signature;
    volatile LONG ReferenceCount;
    volatile LONG State;
    volatile LONG CancelRequested;
    volatile LONG WorkReferenceHeld;
    VIOGPU_WDDM_CONTEXT_SUBMISSION_ENTRY ContextEntry;
    VIOGPU_NATIVE_PASSIVE_WORK Work;
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
    UINT UmdFenceId;
    PGPU_VBUFFER VirtioBuffer;
    VioGpuDod *Adapter;
    PVOID CommandStream;
    UINT CommandStreamSize;
    UINT CommandStreamOffset;
    BOOLEAN PatchApplied;
    BOOLEAN FullyPrepatched;
    UINT AllocationCount;
    VIOGPU_WDDM_SUBMISSION_REFERENCE *References;
};

struct VIOGPU_WDDM_OPEN_ALLOCATION
{
    ULONG Signature;
    VIOGPU_WDDM_ALLOCATION *Allocation;
    VIOGPU_WDDM_DEVICE *Device;
    BOOLEAN ReadOnly;
};

DXGKDDI_QUERYADAPTERINFO VioGpuWddmQueryAdapterInfo;
DXGKDDI_NOTIFY_ACPI_EVENT VioGpuWddmNotifyAcpiEvent;
DXGKDDI_CONTROL_ETW_LOGGING VioGpuWddmControlEtwLogging;
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
DXGKDDI_SETPALETTE VioGpuWddmSetPalette;
DXGKDDI_RENDER VioGpuWddmRender;
DXGKDDI_RENDERKM VioGpuWddmRenderKm;
DXGKDDI_PRESENT VioGpuWddmPresent;
DXGKDDI_PATCH VioGpuWddmPatch;
DXGKDDI_SUBMITCOMMAND VioGpuWddmSubmitCommand;
DXGKDDI_CANCELCOMMAND VioGpuWddmCancelCommand;
DXGKDDI_PREEMPTCOMMAND VioGpuWddmPreemptCommand;
DXGKDDI_QUERYCURRENTFENCE VioGpuWddmQueryCurrentFence;
DXGKDDI_QUERYDEPENDENTENGINEGROUP VioGpuWddmQueryDependentEngineGroup;
DXGKDDI_QUERYENGINESTATUS VioGpuWddmQueryEngineStatus;
DXGKDDI_RESETENGINE VioGpuWddmResetEngine;
DXGKDDI_RESETFROMTIMEOUT VioGpuWddmResetFromTimeout;
DXGKDDI_RESTARTFROMTIMEOUT VioGpuWddmRestartFromTimeout;
DXGKDDI_COLLECTDBGINFO VioGpuWddmCollectDbgInfo;
DXGKDDI_SETVIDPNSOURCEADDRESS VioGpuWddmSetVidPnSourceAddress;
DXGKDDI_GETSCANLINE VioGpuWddmGetScanLine;
DXGKDDI_CONTROLINTERRUPT VioGpuWddmControlInterrupt;
