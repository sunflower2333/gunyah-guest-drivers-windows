// OS/type peers for compiling the actual paging body as ARM64 Windows COFF.
// No mock function body substitutes for ExecuteHostSurfacePaging or its worker.
using ULONG=unsigned long; using UINT=unsigned int; using LONG=long;
using NTSTATUS=long; using ULONGLONG=unsigned long long; using SIZE_T=unsigned long long;
using BOOLEAN=bool; using VOID=void; using BYTE=unsigned char; using PVOID=void*;
using KIRQL=unsigned char; using KSPIN_LOCK=unsigned long long; using KEVENT=int;
#define NULL nullptr
#define TRUE true
#define FALSE false
#define PAGED_CODE() ((void)0)
#define NT_SUCCESS(s) ((s)>=0)
constexpr int STATUS_SUCCESS=0, STATUS_DEVICE_NOT_READY=-1, STATUS_CANCELLED=-2,
    STATUS_INVALID_PARAMETER=-3, STATUS_INVALID_DEVICE_STATE=-4;
constexpr int Executive=0, KernelMode=0, IO_NO_INCREMENT=0;
constexpr UINT VioGpuWddmPagingFlagPageIn=1, VioGpuWddmPagingFlagPageOut=2,
    VioGpuWddmPagingFlagFill=4, VioGpuWddmPagingFlagDiscard=8,
    VioGpuWddmPagingFlagTransferStart=16, VioGpuWddmPagingFlagTransferEnd=32;
constexpr UINT VIOGPU_WDDM_REFERENCE_WRITE=2;
constexpr UINT VIRTIO_GPU_NATIVE_AHB_PAGE_READ=1, VIRTIO_GPU_NATIVE_AHB_PAGE_WRITE=2,
    VIRTIO_GPU_NATIVE_AHB_PAGE_FILL=3, VIRTIO_GPU_NATIVE_AHB_PAGING_MAX_BYTES=65536;
constexpr ULONGLONG VIOGPU_WDDM_HOST_SURFACE_BUDGET=256ULL*1024*1024;
enum VIOGPU_HOST_CONTEXT_RESULT { VioGpuHostContextNotSubmitted, VioGpuHostContextConfirmed };
struct LARGE_INTEGER { long long QuadPart; };
struct VIOGPU_NATIVE_PASSIVE_WORK { volatile LONG *CancelRequested; volatile LONG DisplayReleaseWait; };
struct VIOGPU_WDDM_ALLOCATION {
    bool HostSurface, PlacementValid;
    struct { ULONGLONG Size; } PrivateData;
    ULONGLONG ShareKey, BackingSize, PlacementOffset, Resource2DResetGeneration;
    int LifecycleMutex;
};
struct VioGpuDod {
    BOOLEAN SupportsNativeAhbPaging();
    BOOLEAN IsHardwareResetRequested();
    BOOLEAN TryResumeNativePassiveDispatch(VIOGPU_NATIVE_PASSIVE_WORK*);
    BOOLEAN QueueNativeAhbOperation(UINT,ULONGLONG,ULONGLONG,BOOLEAN,
        VOID(*)(PVOID,VIOGPU_HOST_CONTEXT_RESULT,UINT,ULONGLONG),PVOID);
    VIOGPU_HOST_CONTEXT_RESULT PageNativeAhb(UINT,ULONGLONG,UINT,ULONGLONG,UINT,UINT,PVOID);
    VOID RequestNativeAhbRefreshWork();
    VOID RequestHardwareResetAtAnyIrql();
};
struct VIOGPU_WDDM_PAGING_TRANSACTION {
    VIOGPU_WDDM_ALLOCATION *Allocation; VioGpuDod *Adapter;
    UINT Flags, ResourceId, FillPattern;
    ULONGLONG HostResetGeneration, PlacementOffset, TransferOffset, TransferSize;
    PVOID TransferAddress; volatile LONG CancelRequested; BOOLEAN TransferDataComplete;
};
struct Access { bool Poisoned, PresentPending, Writer, WaitPending; ULONGLONG Sequence, ReleasedSequence; };
struct VIOGPU_WDDM_NATIVE_SHARE_ENTRY {
    bool HostSurface, SurfaceResident, RefreshRequested;
    UINT ResourceId, AllocationReferences, PagingDirection;
    ULONGLONG SurfaceResetGeneration, Size, PagingNextOffset;
    volatile LONG AsyncReferences;
    ::Access Access;
    KEVENT AccessChanged, ProducersIdle;
    VioGpuDod *Adapter;
};
extern KSPIN_LOCK g_VioGpuNativeAccessLock;
extern LONG g_VioGpuNativeImportWorkers;
extern KEVENT g_VioGpuNativeImportWorkersIdle;
BOOLEAN AcquireNativeShareRegistry(BOOLEAN);
VOID ReleaseNativeShareRegistry();
VIOGPU_WDDM_NATIVE_SHARE_ENTRY *FindNativeShareByKeyLocked(VioGpuDod*,ULONGLONG);
LONG InterlockedIncrement(volatile LONG*);
LONG InterlockedDecrement(volatile LONG*);
LONG InterlockedCompareExchange(volatile LONG*,LONG,LONG);
LONG InterlockedExchange(volatile LONG*,LONG);
VOID KeAcquireSpinLock(KSPIN_LOCK*,KIRQL*);
VOID KeReleaseSpinLock(KSPIN_LOCK*,KIRQL);
VOID KeClearEvent(KEVENT*);
VOID KeSetEvent(KEVENT*,int,bool);
NTSTATUS KeWaitForSingleObject(KEVENT*,int,int,bool,LARGE_INTEGER*);
VOID KeReleaseMutex(int*,bool);
BOOLEAN VioGpuNativeAhbCanAccess(const Access*,UINT);
VOID NativeAhbReleaseObserved(PVOID,VIOGPU_HOST_CONTEXT_RESULT,UINT,ULONGLONG);
NTSTATUS AcquireAllocationLifecycle(VIOGPU_WDDM_ALLOCATION*);
VOID PublishStandardPlacement(VIOGPU_WDDM_ALLOCATION*,ULONGLONG);
VOID ClearNativePlacement(VIOGPU_WDDM_ALLOCATION*);
VOID WakeNativeImportWaitersLocked(VioGpuDod*);
struct VIOGPU_WDDM_PAGING_PRIVATE {};
struct VIOGPU_NATIVE_HOST_PAGING_WORK { VIOGPU_WDDM_PAGING_PRIVATE *Batch; PVOID *OrderingOwners; };
VOID NativePagingBatchWorker(PVOID);
// INSERT_FAILURE_RECORD
VIOGPU_HOST_SURFACE_PAGING_FAILURE g_VioGpuHostSurfacePagingFailure;
// INSERT_DECLARATION
// Match the production surrounding pageable section, including at the body.
#pragma code_seg("PAGE")
// INSERT_PRODUCTION
auto volatile keep_paging_worker = &RunNativeHostPagingWork;
