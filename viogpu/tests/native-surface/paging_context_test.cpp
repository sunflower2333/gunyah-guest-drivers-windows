#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
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
using BOOLEAN=bool; using VOID=void; using PVOID=void*; using HANDLE=void*; using NTSTATUS=int;
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
    void ReleaseNativeSubmissionOperation() { ++operationReleases; }
    void QueueNativeSoftwareSubmissionCompletion(UINT,UINT,UINT) { ++completed; }
    void CompleteNativeSystemSubmission(UINT,UINT,UINT) { ++completed; }
    void RequestHardwareResetAtAnyIrql() { ++resets; }
    VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIP CancelNativePassiveWork(VIOGPU_NATIVE_PASSIVE_WORK*) {
        return queued?VioGpuNativePassiveWorkRemoved:VioGpuNativePassiveWorkNotQueued;
    }
};
void NativePagingBatchWorker(void*) {}
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
