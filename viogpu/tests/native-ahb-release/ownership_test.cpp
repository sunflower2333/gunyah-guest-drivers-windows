#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>
#include <functional>
#define VIOGPU_WIRE_U8 uint8_t
#define VIOGPU_WIRE_I8 int8_t
#define VIOGPU_WIRE_U32 uint32_t
#define VIOGPU_WIRE_I32 int32_t
#define VIOGPU_WIRE_U64 uint64_t
#include "viogpu_3d_wire.h"
#include "viogpu_native_ahb_access.h"
#include "viogpu_native_surface_policy.h"

#define _In_
#define _Inout_
#define __declspec(x)
#define PAGED_CODE() ((void)0)
#define RtlZeroMemory(p,n) std::memset(p,0,n)
#define RtlCopyMemory(p,q,n) std::memcpy(p,q,n)
#define TRUE true
#define FALSE false
#define NT_SUCCESS(x) ((x)>=0)
#define DbgPrintEx(...) ((void)0)
using UINT=unsigned; using ULONG=unsigned; using LONG=int; using ULONGLONG=unsigned long long;
using BOOLEAN=bool; using VOID=void; using BYTE=unsigned char; using PVOID=void*;
using PEPROCESS=void*; using NTSTATUS=int; using SIZE_T=std::size_t;
using ULONG_PTR=uintptr_t;
using KIRQL=int; using KSPIN_LOCK=int; using KEVENT=bool;
constexpr int STATUS_SUCCESS=0, STATUS_PENDING=1, STATUS_DEVICE_NOT_READY=-1,
    STATUS_INVALID_PARAMETER=-2, STATUS_NO_MEMORY=-3, STATUS_INVALID_HANDLE=-4,
    STATUS_GRAPHICS_ALLOCATION_BUSY=-5,STATUS_CANCELLED=-6,STATUS_INVALID_DEVICE_STATE=-7;
constexpr int NonPagedPoolNx=0, IO_NO_INCREMENT=0;
void *operator new[](std::size_t size,int) { return ::operator new[](size); }
void *operator new(std::size_t size,int) { return ::operator new(size); }
void KeAcquireSpinLock(KSPIN_LOCK*,KIRQL *irql) { *irql=0; }
void KeReleaseSpinLock(KSPIN_LOCK*,KIRQL) {}
void KeAcquireSpinLockAtDpcLevel(KSPIN_LOCK*) {}
void KeReleaseSpinLockFromDpcLevel(KSPIN_LOCK*) {}
void KeClearEvent(KEVENT *e) { *e=false; }
void KeSetEvent(KEVENT *e,int,bool) { *e=true; }
constexpr int NotificationEvent=0;
void KeInitializeEvent(KEVENT *e,int,bool value) { *e=value; }
LONG InterlockedCompareExchange(volatile LONG *p,LONG value,LONG old) {
    LONG before=*p; if(before==old) *p=value; return before;
}
LONG InterlockedIncrement(volatile LONG *p) { return ++*p; }
LONG InterlockedDecrement(volatile LONG *p) { return --*p; }
LONG InterlockedExchange(volatile LONG *p,LONG v) { LONG old=*p; *p=v; return old; }
struct LIST_ENTRY { LIST_ENTRY *Flink,*Blink; };
using PLIST_ENTRY=LIST_ENTRY*;
#define CONTAINING_RECORD(p,t,f) reinterpret_cast<t*>(reinterpret_cast<char*>(p)-offsetof(t,f))
void InitializeListHead(LIST_ENTRY *l) { l->Flink=l->Blink=l; }
bool IsListEmpty(LIST_ENTRY *l) { return l->Flink==l; }
void InsertTailList(LIST_ENTRY *head,LIST_ENTRY *l) {
    l->Blink=head->Blink; l->Flink=head; head->Blink->Flink=l; head->Blink=l;
}
void RemoveEntryList(LIST_ENTRY *l) { l->Blink->Flink=l->Flink; l->Flink->Blink=l->Blink; }
enum VIOGPU_HOST_CONTEXT_RESULT { VioGpuHostContextNotSubmitted,VioGpuHostContextConfirmed,
    VioGpuHostContextRejected,VioGpuHostContextUnknown };
using VIOGPU_2D_RESOURCE_STATE=int;
struct VIOGPU_NATIVE_PASSIVE_WORK {
    LIST_ENTRY Link{}; bool PipelineEligible{}; volatile LONG DisplayReleaseWait{};
    volatile LONG *CancelRequested{};
    PVOID const *OrderingOwners{}; UINT OrderingOwnerCount{};
};
class VioGpuDod;
struct VIOGPU_WDDM_DEVICE { VioGpuDod *Adapter; };
struct VIOGPU_WDDM_CONTEXT {
    KSPIN_LOCK SubmissionLock{}; LIST_ENTRY PendingSubmissions{};
    VIOGPU_WDDM_DEVICE *Device{};
};
struct VIOGPU_WDDM_ALLOCATION {
    struct { ULONGLONG RequestedIova{},Size{}; } PrivateData;
    ULONGLONG ContextResetGeneration{}; int Pins{};
    bool HostSurface{},Destroying{},PlacementValid{};
    UINT ResourceId{},Signature{},Width{},Height{},Pitch{}; VioGpuDod *Adapter{};
    ULONGLONG ShareKey{},Resource2DResetGeneration{},PlacementOffset{};
    SIZE_T BackingSize{}; int LifecycleMutex{};
};
struct VIOGPU_WDDM_OPEN_ALLOCATION {
    UINT Signature{}; VIOGPU_WDDM_DEVICE *Device{}; VIOGPU_WDDM_ALLOCATION *Allocation{}; bool ReadOnly{};
};
struct DXGK_ALLOCATIONLIST {
    PVOID hDeviceSpecificAllocation{}; UINT SegmentId{},Reserved{},WriteOperation{};
    struct { long long QuadPart{}; } PhysicalAddress;
};
constexpr UINT VIOGPU_WDDM_ALLOCATION_SIGNATURE=1,VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE=2;
constexpr UINT VIOGPU_WDDM_HOST_SURFACE_SEGMENT_ID=2;
constexpr ULONGLONG VIOGPU_WDDM_HOST_SURFACE_BUDGET=256ULL*1024*1024;
bool IsOwnedAllocation(VIOGPU_WDDM_ALLOCATION *a,VioGpuDod *d) {
    return a && a->Signature==VIOGPU_WDDM_ALLOCATION_SIGNATURE && a->Adapter==d;
}
static int lifecycleResult;
int AcquireAllocationLifecycle(VIOGPU_WDDM_ALLOCATION *a) {
    if(lifecycleResult) { int result=lifecycleResult; lifecycleResult=0; return result; }
    assert(a->LifecycleMutex==0); a->LifecycleMutex=1; return 0;
}
void KeReleaseMutex(int *mutex,bool) { assert(*mutex==1); *mutex=0; }
bool EnsureStandard2DAllocationBacking(VIOGPU_WDDM_ALLOCATION *a) { return a->PlacementValid; }
struct LARGE_INTEGER { long long QuadPart; };
constexpr int Executive=0,KernelMode=0;
static std::function<void()> onWait;
static unsigned waitCount;
int KeWaitForSingleObject(KEVENT *event,int,int,bool,LARGE_INTEGER*) {
    if(*event) return 0;
    assert(onWait); ++waitCount; onWait(); return 0;
}
struct VIOGPU_PRIMARY_SCANOUT_LAYOUT { UINT Width,Height,Format,Pitch; SIZE_T Size; };
enum { VioGpuWddmPagingFlagPageIn=1,VioGpuWddmPagingFlagPageOut=2,VioGpuWddmPagingFlagFill=4,
       VioGpuWddmPagingFlagDiscard=8,VioGpuWddmPagingFlagTransferStart=16,VioGpuWddmPagingFlagTransferEnd=32 };
struct VIOGPU_WDDM_PAGING_TRANSACTION {
    VIOGPU_WDDM_ALLOCATION *Allocation{}; VioGpuDod *Adapter{};
    UINT Flags{},ResourceId{},FillPattern{};
    ULONGLONG HostResetGeneration{},PlacementOffset{};
    SIZE_T TransferOffset{},TransferSize{}; PVOID TransferAddress{};
    volatile LONG CancelRequested{}; bool TransferDataComplete{};
};
struct VIOGPU_WDDM_CONTEXT_SUBMISSION_ENTRY { LIST_ENTRY Link; int Kind; PVOID Owner; };
struct VIOGPU_WDDM_SUBMISSION_IMPORT {
    VIOGPU_WDDM_IMPORTED_REFERENCE Reference; PVOID Share,Import;
    VIOGPU_WDDM_ALLOCATION *OwnerAllocation;
};
struct VIOGPU_WDDM_SUBMISSION {
    VioGpuDod *Adapter{}; VIOGPU_WDDM_CONTEXT *Context{};
    UINT ContextId{},UmdFenceId{},ImportCount{}; ULONGLONG ResetGeneration{},FenceId{};
    VIOGPU_WDDM_SUBMISSION_IMPORT *Imports{};
    LIST_ENTRY ImportWaitLink{}; VIOGPU_WDDM_CONTEXT_SUBMISSION_ENTRY ContextEntry{};
    bool ImportWaiting{},ImportsAdmitted{},ImportsHostIssued{};
    volatile LONG State{},CancelRequested{};
    VIOGPU_NATIVE_PASSIVE_WORK Work{}; int References{1};
    PVOID CommandStream{},VirtioBuffer{}; UINT CommandStreamSize{},HostCommandStreamSize{};
};
constexpr int VioGpuWddmSubmissionHostIssued=5,VioGpuWddmSubmissionQuarantined=6;
constexpr int VioGpuWddmContextSubmissionRender=1,VioGpuNativeShareRegistryReady=2;
constexpr unsigned VIOGPU_NATIVE_PIPELINE_WINDOW=8;
bool ReferenceRenderSubmission(VIOGPU_WDDM_SUBMISSION *s) { ++s->References; return true; }
void DereferenceRenderSubmission(VIOGPU_WDDM_SUBMISSION *s) { assert(s->References>1); --s->References; }
int AcquireAllocationSubmissionReference(VIOGPU_WDDM_ALLOCATION *a,VioGpuDod*) { ++a->Pins; return 0; }
void ReleaseAllocationSubmissionReference(VIOGPU_WDDM_ALLOCATION *a) { assert(a->Pins>0); --a->Pins; }
#pragma pack(push,1)
struct VIOGPU_WDDM_MSM_SUBMIT_BO { UINT Flags,Handle; ULONGLONG Presumed; };
struct VIOGPU_WDDM_MSM_SUBMIT_CMD { UINT words[8]; };
#pragma pack(pop)
class VioGpuDod {
public:
    struct Wait { UINT resource; ULONGLONG sequence; PVOID context;
        void (*callback)(PVOID,VIOGPU_HOST_CONTEXT_RESULT,UINT,ULONGLONG); };
    std::vector<Wait> waits;
    std::vector<BYTE> packet;
    std::vector<BYTE> ahb;
    UINT pagingCalls{},failPagingCall{};
    UINT scanouts{},activeScanout{};
    ULONGLONG presentSequence=100;
    ULONGLONG crtcAddress{};
    bool queueOk=true,reset=false;
    BOOLEAN TryResumeNativePassiveDispatch(VIOGPU_NATIVE_PASSIVE_WORK *work);
    bool QueueNativeAhbOperation(UINT id,ULONGLONG,ULONGLONG sequence,bool present,
        void (*cb)(PVOID,VIOGPU_HOST_CONTEXT_RESULT,UINT,ULONGLONG),PVOID context) {
        if(!queueOk) return false;
        if(present) { assert(!sequence); cb(context,VioGpuHostContextConfirmed,id,++presentSequence); return true; }
        waits.push_back({id,sequence,context,cb}); return true;
    }
    bool RefreshNativeSubmit(PVOID,const void *data,UINT size,bool grow) {
        assert(grow); packet.assign(static_cast<const BYTE*>(data),static_cast<const BYTE*>(data)+size); return true;
    }
    bool m_NativePassiveClosing{},m_NativePassiveWorkerRunning{},m_NativePassiveWorkerQueued{};
    KSPIN_LOCK m_NativePassiveLock{};
    VIOGPU_NATIVE_PASSIVE_WORK *m_NativePassiveActiveWork{};
    LIST_ENTRY m_NativePassiveHostPending{},m_NativePassiveQueue{};
    bool IsHardwareResetRequested() { return reset; }
    bool SupportsNativeAhbPaging() const { return true; }
    bool IsNativeAhbScanoutEnabled() const { return true; }
    void SetCrtcVsyncPrimaryAddress(ULONGLONG address) { crtcAddress=address; }
    void LatchFlippedScanout(UINT,UINT,UINT) {}
    bool IsActiveScanoutResource(UINT id) { return id && id==activeScanout; }
    VIOGPU_HOST_CONTEXT_RESULT Set2DScanout(UINT,UINT,UINT,UINT,UINT*,
        VIOGPU_PRIMARY_SCANOUT_LAYOUT*,bool,bool) { ++scanouts; return VioGpuHostContextConfirmed; }
    void RequestHardwareResetAtAnyIrql() { reset=true; }
    VIOGPU_HOST_CONTEXT_RESULT PageNativeAhb(UINT resource,ULONGLONG generation,UINT operation,
        ULONGLONG offset,UINT length,UINT pattern,PVOID data) {
        assert(resource==19 && generation==7 && length && length<=65536 && offset+length<=ahb.size());
        ++pagingCalls;
        if(operation==VIRTIO_GPU_NATIVE_AHB_PAGE_READ) std::memcpy(data,ahb.data()+offset,length);
        else if(operation==VIRTIO_GPU_NATIVE_AHB_PAGE_WRITE) std::memcpy(ahb.data()+offset,data,length);
        else {
            assert(operation==VIRTIO_GPU_NATIVE_AHB_PAGE_FILL && !(offset%4) && !(length%4));
            for(UINT i=0;i<length;i+=4) std::memcpy(ahb.data()+offset+i,&pattern,4);
        }
        return pagingCalls==failPagingCall?VioGpuHostContextUnknown:VioGpuHostContextConfirmed;
    }
    BOOLEAN NativePassiveDispatchReadyLocked(VIOGPU_NATIVE_PASSIVE_WORK *incoming=nullptr);
};
// INSERT_STRUCTS
static LIST_ENTRY g_VioGpuNativeShares,g_VioGpuNativeImports,g_VioGpuNativeAccessWaiters;
static LONG g_VioGpuNativeShareState=VioGpuNativeShareRegistryReady;
static KSPIN_LOCK g_VioGpuNativeAccessLock;
static unsigned wakeCount;
bool AcquireNativeShareRegistry(bool) { return true; }
void ReleaseNativeShareRegistry() {}
void WakeNativeImportWaitersLocked(VioGpuDod*) { ++wakeCount; }
// INSERT_PRODUCTION

static DXGK_ALLOCATIONLIST importList;
static NTSTATUS PinNativeSubmitImports(VIOGPU_WDDM_SUBMISSION *s,const VIOGPU_WDDM_RENDER_COMMAND *h) {
    return PinNativeSubmitImports(s,h,&importList,1);
}

struct Packet {
    VIOGPU_WDDM_RENDER_COMMAND header{};
    VIOGPU_WDDM_IMPORTED_REFERENCE refs[2]{};
};
static Packet make_packet(UINT count=1) {
    Packet p;
    p.header.Flags=VIOGPU_WDDM_RENDER_IMPORTED_REFERENCES;
    p.header.Reserved[0]=sizeof(p.header); p.header.Reserved[1]=count;
    p.header.Reserved[2]=VIOGPU_WDDM_IMPORTED_REFERENCES_VERSION;
    p.header.CommandStreamOffset=sizeof(p.header)+count*sizeof(p.refs[0]);
    p.refs[0]={11,0x10000,0x4000,7,2,1};
    return p;
}
static VIOGPU_WDDM_SUBMISSION make_submit(VioGpuDod &a,VIOGPU_WDDM_CONTEXT &c,UINT fence) {
    VIOGPU_WDDM_SUBMISSION s;
    s.Adapter=&a; s.Context=&c; s.ContextId=4; s.ResetGeneration=7; s.FenceId=s.UmdFenceId=fence;
    s.State=VioGpuWddmSubmissionHostIssued;
    return s;
}
static void link_submit(VIOGPU_WDDM_SUBMISSION &s) {
    InitializeListHead(&s.ImportWaitLink); s.ContextEntry.Kind=VioGpuWddmContextSubmissionRender;
    s.ContextEntry.Owner=&s; InsertTailList(&s.Context->PendingSubmissions,&s.ContextEntry.Link);
}
static void finish(VIOGPU_WDDM_SUBMISSION &s,bool confirmed) {
    RetireNativeSubmitImports(&s,confirmed); UnpinNativeSubmitImports(&s);
    RemoveEntryList(&s.ContextEntry.Link); s.State=VioGpuWddmSubmissionQuarantined;
}
int main() {
    static_assert(sizeof(VIOGPU_WDDM_IMPORTED_REFERENCE)==40);
    InitializeListHead(&g_VioGpuNativeShares); InitializeListHead(&g_VioGpuNativeImports);
    InitializeListHead(&g_VioGpuNativeAccessWaiters);
    VioGpuDod adapter; VIOGPU_WDDM_CONTEXT context{},other{};
    VIOGPU_WDDM_DEVICE device{&adapter}; context.Device=other.Device=&device;
    VIOGPU_WDDM_ALLOCATION wrapper{};
    wrapper.Signature=VIOGPU_WDDM_ALLOCATION_SIGNATURE; wrapper.Adapter=&adapter;
    wrapper.HostSurface=wrapper.PlacementValid=true; wrapper.ShareKey=11;
    wrapper.PrivateData.Size=wrapper.BackingSize=0x4000; wrapper.ResourceId=19;
    wrapper.Resource2DResetGeneration=7;
    VIOGPU_WDDM_OPEN_ALLOCATION opened{VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE,&device,&wrapper,false};
    importList.hDeviceSpecificAllocation=&opened; importList.SegmentId=2; importList.WriteOperation=1;
    InitializeListHead(&context.PendingSubmissions); InitializeListHead(&other.PendingSubmissions);
    InitializeListHead(&adapter.m_NativePassiveHostPending); InitializeListHead(&adapter.m_NativePassiveQueue);
    VIOGPU_WDDM_NATIVE_SHARE_ENTRY share{};
    share.Adapter=&adapter; share.Key=11; share.ResourceId=19; share.Size=0x4000;
    share.HostSurface=true; share.SurfaceResetGeneration=7; share.ProducersIdle=true;
    share.SurfaceResident=true; share.AllocationReferences=1;
    share.Surface.Fourcc=0x34325241U;
    InsertTailList(&g_VioGpuNativeShares,&share.Link);
    VIOGPU_WDDM_NATIVE_IMPORT_ENTRY entry{};
    entry.Adapter=&adapter; entry.Context=&context; entry.Key=11; entry.Iova=0x10000;
    entry.Size=0x4000; entry.ResetGeneration=7; entry.ResourceId=19; entry.HostSurface=true;
    InsertTailList(&g_VioGpuNativeImports,&entry.Link);
    Packet p=make_packet();
    assert(VioGpuNativeImportTableValid(&p.header,64,104));
    p.header.Reserved[1]=257; assert(!VioGpuNativeImportTableValid(&p.header,64,~0ULL)); p=make_packet();
    auto first=make_submit(adapter,context,1); link_submit(first);
    wrapper.ShareKey=12; assert(PinNativeSubmitImports(&first,&p.header)==STATUS_INVALID_HANDLE);
    UnpinNativeSubmitImports(&first); wrapper.ShareKey=11;
    p.refs[0].Iova++; assert(PinNativeSubmitImports(&first,&p.header)==STATUS_INVALID_HANDLE);
    UnpinNativeSubmitImports(&first); p=make_packet();
    assert(PinNativeSubmitImports(&first,&p.header)==0);
    assert(share.SubmissionReferences==1 && entry.SubmissionReferences==1);
    share.SurfaceResident=false;
    assert(AdmitNativeSubmitImports(&first)==STATUS_DEVICE_NOT_READY);
    share.SurfaceResident=true;
    assert(AdmitNativeSubmitImports(&first)==0 && share.Access.Writer && !share.ProducersIdle);
    auto second=make_submit(adapter,context,2); link_submit(second);
    assert(PinNativeSubmitImports(&second,&p.header)==0);
    assert(AdmitNativeSubmitImports(&second)==STATUS_PENDING);
    assert(second.ImportWaiting && second.Work.DisplayReleaseWait);
    first.ImportsHostIssued=true; finish(first,true);
    assert(share.RetiredProducerFence==1 && share.ProducersIdle && !share.Access.Writer);
    // Android still holds this allocation: observing release is mandatory.
    share.Access.Sequence=72; share.Access.ReleasedSequence=0;
    assert(AdmitNativeSubmitImports(&second)==STATUS_PENDING && adapter.waits.size()==1);
    assert(share.Access.WaitPending && !share.Access.Writer);
    VIOGPU_NATIVE_PASSIVE_WORK present{};
    second.Work.PipelineEligible=true;
    InsertTailList(&adapter.m_NativePassiveHostPending,&second.Work.Link);
    assert(adapter.NativePassiveDispatchReadyLocked(&present));
    // A different context/buffer can progress; same-context timestamp cannot.
    auto third=make_submit(adapter,context,3); link_submit(third);
    assert(AdmitNativeSubmitImports(&third)==STATUS_PENDING && third.ImportWaiting);
    auto independent=make_submit(adapter,other,4); link_submit(independent);
    assert(AdmitNativeSubmitImports(&independent)==0);
    finish(independent,true);
    auto release=adapter.waits.back();
    release.callback(release.context,VioGpuHostContextConfirmed,release.resource,release.sequence);
    assert(share.ReleaseCount==1 && share.Access.ReleasedSequence==72 && share.AsyncReferences==0);
    assert(AdmitNativeSubmitImports(&second)==0 && share.Access.Writer);
    assert(!adapter.NativePassiveDispatchReadyLocked(&present));
    second.ImportsHostIssued=true; finish(second,true); RemoveEntryList(&second.Work.Link);
    assert(AdmitNativeSubmitImports(&third)==0); finish(third,true);
    // Host errors or reset cancellation never grant another writer lease.
    auto failed=make_submit(adapter,context,5); link_submit(failed);
    assert(PinNativeSubmitImports(&failed,&p.header)==0);
    assert(AdmitNativeSubmitImports(&failed)==0); failed.ImportsHostIssued=true;
    finish(failed,false); assert(share.Access.Poisoned);
    auto rejected=make_submit(adapter,context,6); link_submit(rejected);
    assert(PinNativeSubmitImports(&rejected,&p.header)==0);
    assert(AdmitNativeSubmitImports(&rejected)==STATUS_DEVICE_NOT_READY); finish(rejected,false);
    assert(share.SubmissionReferences==0 && entry.SubmissionReferences==0);
    // Repacking adds authenticated resources, preserves owned IB indices/data.
    share.Access={}; auto packed=make_submit(adapter,context,7); link_submit(packed);
    assert(PinNativeSubmitImports(&packed,&p.header)==0);
    std::vector<BYTE> raw(sizeof(MSM_CCMD_GEM_SUBMIT_REQ)+sizeof(VIOGPU_WDDM_MSM_SUBMIT_BO)+32);
    auto req=reinterpret_cast<MSM_CCMD_GEM_SUBMIT_REQ*>(raw.data());
    req->nr_bos=1; req->nr_cmds=1; req->hdr.len=raw.size();
    auto bo=reinterpret_cast<VIOGPU_WDDM_MSM_SUBMIT_BO*>(req->payload); bo->Handle=100; bo->Flags=1;
    std::memset(req->payload+sizeof(*bo),0x5a,32);
    packed.CommandStream=req; packed.CommandStreamSize=raw.size();
    assert(RepackNativeSubmitImports(&packed));
    auto out=reinterpret_cast<MSM_CCMD_GEM_SUBMIT_REQ*>(adapter.packet.data());
    auto obs=reinterpret_cast<VIOGPU_WDDM_MSM_SUBMIT_BO*>(out->payload);
    assert(out->nr_bos==2 && obs[0].Handle==100 && obs[1].Handle==19 && obs[1].Flags==2 && obs[1].Presumed==0x10000);
    assert(out->payload[2*sizeof(*obs)]==0x5a && packed.HostCommandStreamSize==raw.size()+sizeof(*bo));
    finish(packed,true);
    assert(IsListEmpty(&g_VioGpuNativeAccessWaiters));
    assert(wrapper.Pins==0);

    // The exact production paging path preserves allocator padding, handles
    // split transfers and returns no fence success after a host sync failure.
    const SIZE_T bytes=0x24000;
    wrapper.PrivateData.Size=wrapper.BackingSize=share.Size=bytes;
    adapter.ahb.resize(bytes); share.Access={}; share.SurfaceResident=false;
    wrapper.PlacementValid=false;
    volatile LONG cancel=0;
    VIOGPU_NATIVE_PASSIVE_WORK pagingWork{};
    pagingWork.CancelRequested=&cancel; pagingWork.PipelineEligible=true;
    InsertTailList(&adapter.m_NativePassiveHostPending,&pagingWork.Link);
    VIOGPU_WDDM_PAGING_TRANSACTION tx{};
    tx.Allocation=&wrapper; tx.Adapter=&adapter; tx.ResourceId=19; tx.HostResetGeneration=7;
    tx.PlacementOffset=0x80000; tx.TransferSize=bytes; tx.FillPattern=0x7b12cd35;
    tx.Flags=VioGpuWddmPagingFlagFill;
    assert(ExecuteHostSurfacePaging(&tx,&pagingWork)==0 && share.SurfaceResident && wrapper.PlacementValid);
    assert(adapter.pagingCalls==3 && wrapper.PlacementOffset==0x80000);
    for(SIZE_T i=0;i<bytes;i+=4) assert(std::memcmp(adapter.ahb.data()+i,&tx.FillPattern,4)==0);
    for(SIZE_T i=0;i<bytes;i++) adapter.ahb[i]=static_cast<BYTE>((i*37)^(i>>9));
    const auto original=adapter.ahb;
    std::vector<BYTE> saved(bytes);
    // Android may keep displaying an evicted surface: eviction and restore
    // are residency bookkeeping that neither waits for release nor copies.
    share.Access.Sequence=73;
    onWait=[&] { assert(!"HostSurface eviction never waits for Android"); };
    const UINT beforeEviction=adapter.pagingCalls;
    tx.Flags=VioGpuWddmPagingFlagPageOut|VioGpuWddmPagingFlagTransferStart;
    tx.TransferAddress=saved.data(); tx.TransferSize=65536;
    assert(ExecuteHostSurfacePaging(&tx,&pagingWork)==0 && waitCount==0);
    assert(!share.SurfaceResident && wrapper.PlacementValid && share.PagingNextOffset==65536);
    tx.Flags=VioGpuWddmPagingFlagPageOut|VioGpuWddmPagingFlagTransferEnd;
    tx.TransferOffset=65536; tx.TransferAddress=saved.data()+65536; tx.TransferSize=bytes-65536;
    assert(ExecuteHostSurfacePaging(&tx,&pagingWork)==0);
    assert(!share.SurfaceResident && !wrapper.PlacementValid && share.PagingDirection==0);
    tx.Flags=VioGpuWddmPagingFlagPageIn|VioGpuWddmPagingFlagTransferStart|VioGpuWddmPagingFlagTransferEnd;
    tx.TransferOffset=0; tx.TransferSize=bytes; tx.TransferAddress=saved.data();
    assert(ExecuteHostSurfacePaging(&tx,&pagingWork)==0 && adapter.ahb==original);
    assert(adapter.pagingCalls==beforeEviction && share.Access.Sequence==73 && !share.Access.WaitPending);
    assert(share.SurfaceResident && wrapper.PlacementValid && !share.Access.Writer);
    lifecycleResult=0x102; // STATUS_TIMEOUT is positive, but owns no mutex.
    assert(PresentResidentHostSurface(&adapter,&wrapper,tx.PlacementOffset,true)==STATUS_DEVICE_NOT_READY);
    assert(!wrapper.Pins && !wrapper.LifecycleMutex);
    // Interleave an already-admitted paging writer with real production Present.
    // The writer needs LifecycleMutex before it can signal ProducersIdle.
    share.Access.ReleasedSequence=share.Access.Sequence;
    share.Access.Writer=true; share.ProducersIdle=false;
    const auto beforePresent=adapter.scanouts;
    onWait=[&] {
        assert(share.Access.PresentPending && share.Access.Writer && wrapper.Pins==1);
        assert(AcquireAllocationLifecycle(&wrapper)==0);
        share.SurfaceResident=false;
        wrapper.PlacementValid=false;
        share.Access.Writer=false;
        KeSetEvent(&share.ProducersIdle,0,false);
        KeReleaseMutex(&wrapper.LifecycleMutex,false);
    };
    assert(PresentResidentHostSurface(&adapter,&wrapper,tx.PlacementOffset,true)==STATUS_DEVICE_NOT_READY);
    assert(adapter.scanouts==beforePresent && wrapper.Pins==0 && !wrapper.LifecycleMutex);
    assert(share.Access.Poisoned && adapter.reset);
    adapter.reset=false; share.Access={}; share.SurfaceResident=true; wrapper.PlacementValid=true;
    // A normal resident Present retains its allocation until host acceptance.
    assert(PresentResidentHostSurface(&adapter,&wrapper,tx.PlacementOffset,true)==0);
    assert(adapter.scanouts==beforePresent+1 && wrapper.Pins==0 && adapter.crtcAddress==tx.PlacementOffset);
    // Android still reads the accepted present. As the current scanout it is
    // already on screen: a repeat is not sent, and nothing is poisoned.
    const ULONGLONG shown=share.Access.Sequence;
    const auto waitsBefore=adapter.waits.size();
    assert(shown && shown!=share.Access.ReleasedSequence);
    adapter.activeScanout=19;
    onWait=[&] { assert(!"a front buffer is never released while it is shown"); };
    assert(PresentResidentHostSurface(&adapter,&wrapper,tx.PlacementOffset,false)==0);
    assert(adapter.scanouts==beforePresent+1 && adapter.waits.size()==waitsBefore);
    assert(!share.Access.Poisoned && !adapter.reset && share.Access.Sequence==shown && !share.AsyncReferences);
    // Held but no longer scanned out: the repeat waits for that release.
    adapter.activeScanout=0;
    onWait=[&] {
        assert(share.Access.WaitPending && !share.Access.PresentPending && adapter.waits.size()==waitsBefore+1);
        const auto release=adapter.waits.back();
        release.callback(release.context,VioGpuHostContextConfirmed,release.resource,release.sequence);
    };
    assert(PresentResidentHostSurface(&adapter,&wrapper,tx.PlacementOffset,false)==0);
    assert(adapter.scanouts==beforePresent+2 && share.Access.ReleasedSequence==shown);
    assert(share.Access.Sequence!=shown && !share.Access.Poisoned && !adapter.reset && !share.AsyncReferences);
    share.Access.Sequence=share.Access.ReleasedSequence=0;
    const UINT beforeDiscard=adapter.pagingCalls;
    tx.Flags=VioGpuWddmPagingFlagDiscard; tx.TransferSize=0; tx.TransferAddress=nullptr;
    assert(ExecuteHostSurfacePaging(&tx,&pagingWork)==0 && adapter.pagingCalls==beforeDiscard);
    assert(!share.SurfaceResident && !wrapper.PlacementValid && adapter.ahb==original);
    tx.Flags=VioGpuWddmPagingFlagFill; tx.TransferSize=bytes;
    adapter.failPagingCall=adapter.pagingCalls+2;
    assert(ExecuteHostSurfacePaging(&tx,&pagingWork)==STATUS_DEVICE_NOT_READY);
    assert(share.Access.Poisoned && !share.Access.Writer && adapter.reset);
    assert(!share.SurfaceResident && !wrapper.PlacementValid && share.AsyncReferences==0);
    adapter.reset=false; share.Access={};
    lifecycleResult=0x102;
    const UINT beforeTimeout=adapter.pagingCalls;
    assert(ExecuteHostSurfacePaging(&tx,&pagingWork)==STATUS_DEVICE_NOT_READY);
    assert(adapter.pagingCalls==beforeTimeout && share.Access.Poisoned && adapter.reset);
    assert(!wrapper.LifecycleMutex && !share.AsyncReferences);
    RemoveEntryList(&pagingWork.Link);

    // OS workers need not run in ExQueueWorkItem order. A later eviction
    // starts first while the initial fill has not acquired its writer lease.
    for (UINT operation : {VioGpuWddmPagingFlagPageOut,VioGpuWddmPagingFlagDiscard}) {
        adapter.reset=false; adapter.failPagingCall=0; share.Access={};
        share.SurfaceResident=false; wrapper.PlacementValid=false;
        share.PagingDirection=0; share.PagingNextOffset=0;
        VIOGPU_NATIVE_PASSIVE_WORK initial{},eviction{};
        PVOID owners[]={&wrapper};
        initial.OrderingOwners=eviction.OrderingOwners=owners;
        initial.OrderingOwnerCount=eviction.OrderingOwnerCount=1;
        initial.CancelRequested=eviction.CancelRequested=&cancel;
        initial.PipelineEligible=eviction.PipelineEligible=true;
        initial.DisplayReleaseWait=eviction.DisplayReleaseWait=true;
        InsertTailList(&adapter.m_NativePassiveHostPending,&initial.Link);
        InsertTailList(&adapter.m_NativePassiveHostPending,&eviction.Link);
        auto fill=tx;
        fill.Flags=VioGpuWddmPagingFlagFill; fill.TransferOffset=0; fill.TransferSize=bytes;
        fill.TransferAddress=nullptr; fill.TransferDataComplete=false;
        auto evict=fill;
        evict.Flags=operation;
        evict.TransferSize=operation==VioGpuWddmPagingFlagDiscard?0:bytes;
        evict.TransferAddress=operation==VioGpuWddmPagingFlagDiscard?nullptr:saved.data();
        if(operation==VioGpuWddmPagingFlagPageOut)
            evict.Flags|=VioGpuWddmPagingFlagTransferStart|VioGpuWddmPagingFlagTransferEnd;
        const auto beforeWait=waitCount;
        onWait=[&] {
            assert(!share.Access.Writer && !adapter.reset);
            assert(adapter.NativePassiveDispatchReadyLocked(&present));
            assert(ExecuteHostSurfacePaging(&fill,&initial)==STATUS_SUCCESS);
            assert(share.SurfaceResident && wrapper.PlacementValid);
            assert(!adapter.TryResumeNativePassiveDispatch(&eviction));
            RemoveEntryList(&initial.Link);
        };
        assert(ExecuteHostSurfacePaging(&evict,&eviction)==STATUS_SUCCESS);
        assert(waitCount==beforeWait+1 && !share.SurfaceResident && !wrapper.PlacementValid);
        assert(!share.Access.Poisoned && !adapter.reset && !share.AsyncReferences);
        for(SIZE_T i=0;i<bytes;i+=4) assert(std::memcmp(adapter.ahb.data()+i,&fill.FillPattern,4)==0);
        RemoveEntryList(&eviction.Link);
    }

    // An Android release wait on a different allocation is not a global
    // barrier: another paging worker and independent Render can be admitted.
    VIOGPU_WDDM_ALLOCATION unrelated{};
    PVOID otherOwner[]={&unrelated},currentOwner[]={&wrapper};
    VIOGPU_NATIVE_PASSIVE_WORK held{},otherPaging{},render{};
    held.OrderingOwners=otherOwner; otherPaging.OrderingOwners=currentOwner;
    held.OrderingOwnerCount=otherPaging.OrderingOwnerCount=1;
    held.DisplayReleaseWait=otherPaging.DisplayReleaseWait=true;
    InsertTailList(&adapter.m_NativePassiveHostPending,&held.Link);
    InsertTailList(&adapter.m_NativePassiveHostPending,&otherPaging.Link);
    assert(adapter.TryResumeNativePassiveDispatch(&otherPaging));
    assert(held.DisplayReleaseWait && !otherPaging.DisplayReleaseWait);
    assert(adapter.TryResumeNativePassiveDispatch(&render));
    RemoveEntryList(&held.Link); RemoveEntryList(&otherPaging.Link);
}
