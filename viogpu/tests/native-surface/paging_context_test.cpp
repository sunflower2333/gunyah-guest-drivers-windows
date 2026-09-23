#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <new>
#define _In_
#define _In_opt_
#define _Inout_
#define _Out_
#define _Use_decl_annotations_
#define APIENTRY
#define CONST const
#define TRUE true
#define FALSE false
#define NT_SUCCESS(x) ((x)>=0)
#define NT_ASSERT(x) assert(x)
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define KeMemoryBarrier() ((void)0)
using UINT=unsigned; using ULONG=uint32_t; using USHORT=uint16_t; using LONG=int;
using ULONGLONG=uint64_t; using SIZE_T=size_t; using BYTE=unsigned char;
using ULONG_PTR=uintptr_t;
using BOOLEAN=bool; using VOID=void; using PVOID=void*; using HANDLE=void*; using NTSTATUS=int;
using KIRQL=int;
enum Pool { NonPagedPoolNx };
void *operator new(size_t size,Pool) { return ::operator new(size,std::nothrow); }
void *operator new[](size_t size,Pool) { return ::operator new[](size,std::nothrow); }
struct WORK_QUEUE_ITEM { void (*Routine)(void*){}; void *Context{}; };
constexpr int DelayedWorkQueue=0,IO_NO_INCREMENT=0;
static WORK_QUEUE_ITEM *queuedHostWork;
static int g_VioGpuNativeAccessLock,g_VioGpuNativeImportWorkers;
static bool g_VioGpuNativeImportWorkersIdle=true;
void KeAcquireSpinLock(int *lock,KIRQL*) { assert(!*lock);*lock=1; }
void KeReleaseSpinLock(int *lock,KIRQL) { assert(*lock);*lock=0; }
void KeSetEvent(bool *event,int,bool) { *event=true; }
void KeClearEvent(bool *event) { *event=false; }
void ExInitializeWorkItem(WORK_QUEUE_ITEM *work,void (*routine)(void*),void *context) {
    work->Routine=routine;work->Context=context;
}
void ExQueueWorkItem(WORK_QUEUE_ITEM *work,int) { assert(!queuedHostWork);queuedHostWork=work; }
constexpr UINT MAXUINT=~0U,MAXULONG=~0U,PAGE_SIZE=4096,VIOGPU_NATIVE_RESOURCE_ID_START=0x80000000;
constexpr ULONGLONG MAXULONGLONG=~0ULL;
constexpr int STATUS_SUCCESS=0;
constexpr ULONG VIOGPU_WDDM_DMA_SIGNATURE=1,VIOGPU_WDDM_PAGING_DMA_SIGNATURE=2,
    VIOGPU_WDDM_PAGING_TRANSACTION_SIGNATURE=3,VIOGPU_WDDM_CONTEXT_SIGNATURE=4,VIOGPU_WDDM_DEVICE_SIGNATURE=5;
enum { DXGK_OPERATION_FILL=1,DXGK_OPERATION_TRANSFER=2,DXGK_OPERATION_DISCARD_CONTENT=3 };
LONG InterlockedCompareExchange(volatile LONG *p,LONG n,LONG old) { LONG r=*p;if(r==old)*p=n;return r; }
LONG InterlockedExchange(volatile LONG *p,LONG n) { LONG r=*p;*p=n;return r; }
struct Rundown { int refs{}; bool closed{}; };
bool ExAcquireRundownProtection(Rundown *r) { if(r->closed)return false;++r->refs;return true; }
void ExReleaseRundownProtection(Rundown *r) { assert(r->refs>0);--r->refs; }
struct LIST_ENTRY { LIST_ENTRY *Flink{},*Blink{}; };
using PLIST_ENTRY=LIST_ENTRY*;
struct VIOGPU_NATIVE_PASSIVE_WORK {
    LIST_ENTRY Link; void (*Routine)(void*){}; void (*CancelRoutine)(void*){};
    void *Context{}; volatile LONG *CancelRequested{};
    PVOID const *OrderingOwners{}; UINT OrderingOwnerCount{};
    bool PipelineEligible{}; volatile LONG DisplayReleaseWait{};
};
class VioGpuDod;
struct VIOGPU_WDDM_ALLOCATION { bool HostSurface=true; int refs=1; };
// INSERT_RECORDS
struct VIOGPU_WDDM_DEVICE { ULONG Signature=VIOGPU_WDDM_DEVICE_SIGNATURE; VioGpuDod *Adapter{}; };
struct VIOGPU_WDDM_CONTEXT {
    ULONG Signature=VIOGPU_WDDM_CONTEXT_SIGNATURE; Rundown Operations;
    VIOGPU_WDDM_DEVICE *Device{}; UINT NodeOrdinal=0,EngineAffinity=1;
};
struct Command {
    union { HANDLE hContext{}; HANDLE hDevice; };
    struct { UINT Value=1; } Flags;
    void *pDmaBuffer{},*pDmaBufferPrivateData{};
    UINT DmaBufferSize{},DmaBufferSubmissionStartOffset{},DmaBufferSubmissionEndOffset{};
    UINT DmaBufferPrivateDataSize{},DmaBufferPrivateDataSubmissionStartOffset{},DmaBufferPrivateDataSubmissionEndOffset{};
    UINT SubmissionFenceId=7,NodeOrdinal{},EngineOrdinal{};
    void *pAllocationList{},*pPatchLocationList{};
    UINT AllocationListSize{},PatchLocationListSize{},PatchLocationListSubmissionStart{},PatchLocationListSubmissionLength{};
};
using DXGKARG_CANCELCOMMAND=Command;
enum VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIP { VioGpuNativePassiveWorkNotQueued,VioGpuNativePassiveWorkRemoved };
class VioGpuDod {
public:
    unsigned queued{},resets{},completed{},retired{},operationReleases{};
    bool IsNativeContextGenerationCurrent(LONG,ULONGLONG) { return true; }
    bool QueueNativePassiveWork(VIOGPU_NATIVE_PASSIVE_WORK*,UINT) { ++queued;return true; }
    void ReleaseNativePassiveDispatch(VIOGPU_NATIVE_PASSIVE_WORK *work) {
        assert(work->OrderingOwnerCount==3 && work->OrderingOwners && work->PipelineEligible && work->DisplayReleaseWait);
        assert(g_VioGpuNativeImportWorkers==1 && !g_VioGpuNativeImportWorkersIdle);
        ++released;
    }
    unsigned released{};
    void ReleaseNativeSubmissionOperation() { ++operationReleases; }
    void QueueNativeSoftwareSubmissionCompletion(UINT,UINT,UINT) { ++completed; }
    void CompleteNativeSystemSubmission(UINT,UINT,UINT) { ++completed; }
    void RequestHardwareResetAtAnyIrql() { ++resets; }
    VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIP CancelNativePassiveWork(VIOGPU_NATIVE_PASSIVE_WORK*) {
        return queued?VioGpuNativePassiveWorkRemoved:VioGpuNativePassiveWorkNotQueued;
    }
};
void NativePagingBatchWorker(void *opaque) {
    auto first=static_cast<VIOGPU_WDDM_PAGING_PRIVATE*>(opaque);
    assert(first->BatchDetached && first->Work.OrderingOwnerCount==3);
    for(unsigned i=0;i<3;++i) assert(first->Work.OrderingOwners[i]);
    // Completion can clear private metadata and publish the fence. The outer
    // worker owns the vector independently and must not touch the batch again.
    *first={};
}
bool ReleaseAllocationSubmissionReference(VIOGPU_WDDM_ALLOCATION *a) { assert(a->refs>0);--a->refs;return true; }
void RetirePatchDmaOwner(VioGpuDod *a,const Command*) { ++a->retired; }
// INSERT_PRODUCTION
static int Patch(VioGpuDod *adapter,const Command *patchArguments) {
    // INSERT_PATCH
}
static int Submit(VioGpuDod *adapter,const Command *submitCommand) {
    const UINT privateStart=submitCommand->DmaBufferPrivateDataSubmissionStartOffset;
    const UINT privateEnd=submitCommand->DmaBufferPrivateDataSubmissionEndOffset;
    auto privateData=reinterpret_cast<VIOGPU_WDDM_KMD_DMA_PRIVATE*>(
        static_cast<BYTE*>(submitCommand->pDmaBufferPrivateData)+privateStart);
    NTSTATUS status=STATUS_SUCCESS;
    // INSERT_SUBMIT
}
struct Fixture {
    VioGpuDod adapter,foreignAdapter;
    VIOGPU_WDDM_DEVICE device{VIOGPU_WDDM_DEVICE_SIGNATURE,&adapter};
    VIOGPU_WDDM_CONTEXT context;
    VIOGPU_WDDM_ALLOCATION allocation;
    VIOGPU_WDDM_PAGING_DMA_PACKET packet{};
    VIOGPU_WDDM_PAGING_PRIVATE p{};
    Command command;
    Fixture() {
        context.Device=&device;
        packet.Signature=VIOGPU_WDDM_PAGING_DMA_SIGNATURE;packet.Version=VioGpuWddmDmaPrivateVersion;
        packet.Size=sizeof(packet);packet.Operation=DXGK_OPERATION_FILL;
        packet.Flags=VioGpuWddmPagingFlagFill|VioGpuWddmPagingFlagHostSurface;
        packet.ResourceId=23;packet.PlacementOffset=0x80000;packet.TransferSize=8192;
        p.Header.Signature=VIOGPU_WDDM_DMA_SIGNATURE;p.Header.Version=VioGpuWddmDmaPrivateVersion;
        p.Header.Kind=VioGpuWddmDmaKindPaging;p.Header.DmaBuffer=&packet;p.Header.DmaBufferSize=sizeof(packet);
        p.Header.CommandLength=sizeof(packet);p.Header.Packet=&packet;p.Header.PacketLength=sizeof(packet);
        p.Header.Submission=&p;p.Header.Flags=packet.Flags;
        p.Transaction.Signature=VIOGPU_WDDM_PAGING_TRANSACTION_SIGNATURE;
        p.Transaction.State=VioGpuWddmPagingTransactionBuilt;p.Transaction.ReferenceHeld=1;
        p.Transaction.Adapter=&adapter;p.Transaction.Allocation=&allocation;
        p.Transaction.Operation=packet.Operation;p.Transaction.Flags=packet.Flags;
        p.Transaction.ResourceId=packet.ResourceId;p.Transaction.TransferSize=packet.TransferSize;
        p.Transaction.PlacementOffset=packet.PlacementOffset;p.Transaction.HostResetGeneration=7;
        p.Work.Link.Flink=p.Work.Link.Blink=&p.Work.Link;
        p.Work.Routine=NativePagingBatchWorker;p.Work.Context=&p;
        p.Work.CancelRequested=&p.Transaction.CancelRequested;
        command.hContext=&context;command.pDmaBuffer=&packet;command.DmaBufferSize=sizeof(packet);
        command.DmaBufferSubmissionEndOffset=sizeof(packet);command.pDmaBufferPrivateData=&p;
        command.DmaBufferPrivateDataSize=sizeof(p);command.DmaBufferPrivateDataSubmissionEndOffset=sizeof(p);
    }
    void accepted() {
        const auto before=context;
        assert(Patch(&adapter,&command)==0 && !adapter.retired);
        assert(Submit(&adapter,&command)==0 && adapter.queued==1 && !adapter.resets);
        assert(p.Transaction.State==VioGpuWddmPagingTransactionQueued && allocation.refs==1);
        assert(!context.Operations.refs && std::memcmp(&before,&context,sizeof(context))==0);
        assert(VioGpuWddmCancelCommand(&adapter,&command)==0);
        assert(p.Transaction.State==VioGpuWddmPagingTransactionCancelled && !allocation.refs);
        assert(adapter.completed==1 && adapter.resets==1 && !context.Operations.refs);
    }
    void rejected() {
        assert(Patch(&adapter,&command)==0 && adapter.retired==1);
        assert(Submit(&adapter,&command)==0 && !adapter.queued && adapter.resets==1);
        assert(!context.Operations.refs);
    }
};
int main() {
    {
        VIOGPU_WDDM_ALLOCATION owners[3];
        VIOGPU_WDDM_PAGING_PRIVATE records[4]{};
        records[0].Transaction.Allocation=&owners[2];
        records[1].Transaction.Allocation=&owners[0];
        records[2].Transaction.Allocation=&owners[2];
        records[3].Transaction.Allocation=&owners[1];
        records[0].BatchPrivateData=records;
        records[0].BatchPrivateEnd=records[0].BatchPrivateDataSize=sizeof(records);
        PVOID captured[4]{};
        assert(CaptureNativeHostPagingOwners(records,captured,4)==3);
        for(unsigned i=0;i<3;++i) assert(captured[i]==&owners[i]);
        assert(CaptureNativeHostPagingOwners(records,captured,3)==0);
        records[0].BatchPrivateEnd--;
        assert(CaptureNativeHostPagingOwners(records,captured,4)==0);
        assert(!DetachNativeHostPagingBatch(records) && !queuedHostWork);
        records[0].BatchPrivateEnd++;
        VioGpuDod adapter;
        records[0].Transaction.Adapter=&adapter;
        assert(DetachNativeHostPagingBatch(records) && adapter.released==1 && queuedHostWork);
        assert(records[0].Work.OrderingOwnerCount==3);
        for(unsigned i=0;i<3;++i) assert(records[0].Work.OrderingOwners[i]==&owners[i]);
        auto work=queuedHostWork;queuedHostWork=nullptr;
        work->Routine(work->Context);
        assert(!g_VioGpuNativeImportWorkers && g_VioGpuNativeImportWorkersIdle);
    }
    { Fixture f; f.accepted(); }
    { Fixture f; f.command.hContext=nullptr; f.accepted(); }
    { Fixture f; f.device.Adapter=&f.foreignAdapter; f.rejected(); }
    { Fixture f; f.context.Operations.closed=true; f.rejected(); }
    { Fixture f; f.command.hDevice=&f.device; f.rejected(); }
    { Fixture f; f.p.Transaction.Adapter=&f.foreignAdapter; f.rejected(); }
    { Fixture f; f.p.Transaction.ReferenceHeld=0; f.rejected(); }
    { Fixture f; f.packet.ResourceId++; f.rejected(); }
    { Fixture f; f.context.NodeOrdinal=1; f.rejected(); }
    { Fixture f; f.context.EngineAffinity=2; f.rejected(); }
    { Fixture f; f.command.hContext=nullptr; f.p.Header.Flags=0; f.rejected(); }
    { Fixture f; f.device.Adapter=&f.foreignAdapter; assert(VioGpuWddmCancelCommand(&f.adapter,&f.command)==0);
      assert(f.allocation.refs==1 && f.adapter.resets==1 && !f.context.Operations.refs); }
}
