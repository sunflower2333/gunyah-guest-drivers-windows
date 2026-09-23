#include <cassert>
#include <cstdint>
#include <cstring>
#include "viogpu_wddm_abi.h"
#define _In_
#define PAGED_CODE() ((void)0)
#define __try try
#define __except(x) catch (...)
#define RtlCopyMemory(p,q,n) std::memcpy(p,q,n)
#define TRUE true
#define FALSE false
#define NT_SUCCESS(x) ((x)>=0)
template<class... T> void DbgPrintEx(T... args) { ((void)args,...); }
constexpr int DPFLTR_IHVVIDEO_ID=0,DPFLTR_ERROR_LEVEL=0;
using UINT=unsigned; using ULONG_PTR=uintptr_t; using BYTE=unsigned char;
using LONG=int;
LONG InterlockedIncrement(volatile LONG *p) { return ++*p; }
using NTSTATUS=int; using BOOLEAN=bool; using HANDLE=void*;
using D3DKMT_HANDLE=uint32_t;
using DXGKARG_RELEASE_HANDLE=void*;
constexpr NTSTATUS STATUS_SUCCESS=0,STATUS_INVALID_PARAMETER=-1,STATUS_INVALID_USER_BUFFER=-2,
    STATUS_NOT_SUPPORTED=-3,STATUS_DEVICE_NOT_READY=-4,STATUS_INVALID_HANDLE=-5;
constexpr UINT VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE=101,VIOGPU_WDDM_RESOURCE_SIGNATURE=102;
enum DXGK_HANDLE_TYPE { DXGK_HANDLE_ALLOCATION=1,DXGK_HANDLE_RESOURCE=2 };
struct DXGKARGCB_GETHANDLEDATA {
    D3DKMT_HANDLE hObject{}; DXGK_HANDLE_TYPE Type{};
    union { UINT Value{}; struct { UINT DeviceSpecific:1; }; } Flags;
};
struct DXGKARGCB_RELEASEHANDLEDATA { DXGKARG_RELEASE_HANDLE ReleaseHandle{}; DXGK_HANDLE_TYPE Type{}; };
struct DXGK_INTERFACE {
    void *(*DxgkCbAcquireHandleData)(const DXGKARGCB_GETHANDLEDATA*,DXGKARG_RELEASE_HANDLE*);
    void (*DxgkCbReleaseHandleData)(DXGKARGCB_RELEASEHANDLEDATA);
    D3DKMT_HANDLE (*DxgkCbGetHandleParent)(D3DKMT_HANDLE);
};
class VioGpuDod {
public:
    DXGK_INTERFACE dxgk{}; bool active=true,reset=false;
    DXGK_INTERFACE *GetDxgkInterface() { return &dxgk; }
    bool IsDriverActive() { return active; }
    bool IsHardwareResetRequested() { return reset; }
    void RecordNativeSurfaceResourceDiagnostic(UINT,UINT,NTSTATUS) {}
};
struct VIOGPU_WDDM_DEVICE { VioGpuDod *Adapter{}; int References{}; bool Closing{}; };
struct VIOGPU_WDDM_RESOURCE { UINT Signature=VIOGPU_WDDM_RESOURCE_SIGNATURE; VioGpuDod *Adapter{}; int Count=1; };
struct VIOGPU_WDDM_ALLOCATION {
    VioGpuDod *Adapter{}; VIOGPU_WDDM_RESOURCE *Resource{};
    bool HostSurface=true,Destroying=false;
    uint64_t ShareKey=11,BackingSize=8192,Resource2DResetGeneration=7;
    struct { uint64_t Size=8192; } PrivateData;
    UINT ResourceId=23; int LifecycleMutex{},References{};
};
struct VIOGPU_WDDM_OPEN_ALLOCATION {
    UINT Signature=VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE; bool ReadOnly=false;
    VIOGPU_WDDM_DEVICE *Device{}; VIOGPU_WDDM_ALLOCATION *Allocation{};
};
struct Share {
    bool HostSurface=true,OwnerReleased=false; int OwnerProcess=1;
    UINT AllocationReferences=1,ResourceId=23; uint64_t Size=8192,SurfaceResetGeneration=7;
};
struct DXGKARG_ESCAPE {
    HANDLE hDevice{},hContext{}; struct { UINT Value{}; } Flags;
    void *pPrivateDriverData{}; UINT PrivateDriverDataSize{};
};
static Share share;
static int lifecycleStatus,allocationPins,resourcePins,parentCalls;
static bool failLookup,wrongResource,registryAvailable=true;
static D3DKMT_HANDLE parentValue=0x40008010;
static VIOGPU_WDDM_OPEN_ALLOCATION *opened;
bool IsCurrentAbiHeader(const VIOGPU_WDDM_ABI_HEADER *h,UINT size) {
    return h->Magic==VIOGPU_WDDM_ABI_MAGIC && h->Version==0 && h->Size==size && !h->Reserved;
}
bool ReferenceDevice(VIOGPU_WDDM_DEVICE *d) { if(d->Closing) return false; ++d->References; return true; }
void DereferenceDevice(VIOGPU_WDDM_DEVICE *d) { assert(d->References>0); --d->References; }
bool IsOwnedAllocation(VIOGPU_WDDM_ALLOCATION *a,VioGpuDod *d) { return a && a->Adapter==d; }
int AcquireAllocationLifecycle(VIOGPU_WDDM_ALLOCATION *a) {
    if(lifecycleStatus) return lifecycleStatus;
    assert(!a->LifecycleMutex); a->LifecycleMutex=1; return 0;
}
void KeReleaseMutex(int *m,bool) { assert(*m==1); *m=0; }
bool AcquireNativeShareRegistry(bool) { return registryAvailable; }
void ReleaseNativeShareRegistry() {}
Share *FindNativeShareByKeyLocked(VioGpuDod*,uint64_t key) { return key==11?&share:nullptr; }
int PsGetCurrentProcess() { return 1; }
int AcquireAllocationSubmissionReference(VIOGPU_WDDM_ALLOCATION *a,VioGpuDod*) { ++a->References; return 0; }
void ReleaseAllocationSubmissionReference(VIOGPU_WDDM_ALLOCATION *a) { assert(a->References>0); --a->References; }
int ReadResourceAllocationCount(VIOGPU_WDDM_RESOURCE *r) { return r->Count; }
void *Acquire(const DXGKARGCB_GETHANDLEDATA *q,DXGKARG_RELEASE_HANDLE *pin) {
    if(q->Type==DXGK_HANDLE_ALLOCATION) {
        assert(q->Flags.Value==1 && q->hObject==0x40001240);
        if(failLookup) return nullptr;
        ++allocationPins; *pin=reinterpret_cast<void*>(1); return opened;
    }
    assert(q->Type==DXGK_HANDLE_RESOURCE && !q->Flags.Value && allocationPins==1 && opened->Allocation->References==1);
    assert(q->hObject==parentValue);
    ++resourcePins; *pin=reinterpret_cast<void*>(2);
    static VIOGPU_WDDM_RESOURCE unrelated;
    unrelated.Adapter=opened->Allocation->Adapter;
    return wrongResource?&unrelated:opened->Allocation->Resource;
}
void Release(DXGKARGCB_RELEASEHANDLEDATA r) {
    if(r.Type==DXGK_HANDLE_ALLOCATION) { assert(r.ReleaseHandle==reinterpret_cast<void*>(1)); --allocationPins; }
    else { assert(r.Type==DXGK_HANDLE_RESOURCE && r.ReleaseHandle==reinterpret_cast<void*>(2)); --resourcePins; }
}
D3DKMT_HANDLE Parent(D3DKMT_HANDLE h) {
    assert(h==0x40001240 && allocationPins==1 && opened->Device->References==1);
    assert(opened->Allocation->References==1 && !opened->Allocation->LifecycleMutex);
    ++parentCalls; return parentValue;
}
// INSERT_QUERY
int main() {
    VioGpuDod adapter,other;
    adapter.dxgk={Acquire,Release,Parent};
    VIOGPU_WDDM_DEVICE device{&adapter},foreign{&other};
    VIOGPU_WDDM_RESOURCE resource; resource.Adapter=&adapter;
    VIOGPU_WDDM_ALLOCATION allocation; allocation.Adapter=&adapter; allocation.Resource=&resource;
    VIOGPU_WDDM_OPEN_ALLOCATION open; open.Device=&device; open.Allocation=&allocation; opened=&open;
    VIOGPU_WDDM_NATIVE_SURFACE_RESOURCE original{};
    original.Header={VIOGPU_WDDM_ABI_MAGIC,0,sizeof(original),0};
    original.Opcode=VIOGPU_WDDM_ESCAPE_QUERY_NATIVE_SURFACE_RESOURCE;
    original.AllocationHandle=0x40001240; original.ShareKey=11; original.Size=8192; original.ResetGeneration=7;
    auto request=original;
    DXGKARG_ESCAPE escape; escape.pPrivateDriverData=&request; escape.PrivateDriverDataSize=sizeof(request);
    auto run=[&](bool success) {
        auto before=request;
        const auto status=QueryNativeSurfaceResource(&adapter,&escape);
        assert((status==0)==success);
        if(success) { assert(request.ResourceHandle==parentValue); before.ResourceHandle=request.ResourceHandle; }
        assert(std::memcmp(&before,&request,sizeof(request))==0);
        assert(!allocationPins && !resourcePins && !device.References && !foreign.References);
        assert(!allocation.References && !allocation.LifecycleMutex);
        request=original;
    };
    run(true); escape.hDevice=&device; run(true); escape.hDevice=&foreign; run(false); escape.hDevice=nullptr;
    failLookup=true; run(false); failLookup=false;
    open.Device=&foreign; run(false); open.Device=&device;
    open.ReadOnly=true; run(false); open.ReadOnly=false;
    device.Closing=true; run(false); device.Closing=false;
    allocation.HostSurface=false; run(false); allocation.HostSurface=true;
    allocation.Destroying=true; run(false); allocation.Destroying=false;
    request.ShareKey++; run(false); request.Size++; run(false); request.ResetGeneration++; run(false);
    request.ResourceHandle=1; run(false); request.Flags=1; run(false); request.Reserved=1; run(false);
    share.OwnerProcess=2; run(false); share.OwnerProcess=1;
    share.OwnerReleased=true; run(false); share.OwnerReleased=false;
    share.AllocationReferences=2; run(false); share.AllocationReferences=1;
    registryAvailable=false; run(false); registryAvailable=true;
    wrongResource=true; run(false); wrongResource=false;
    parentValue=0; run(false); parentValue=UINT32_MAX; run(true); parentValue=0x40008010;
    resource.Count=2; run(false); resource.Count=1;
    adapter.dxgk.DxgkCbGetHandleParent=nullptr; run(false); adapter.dxgk.DxgkCbGetHandleParent=Parent;
    lifecycleStatus=0x102; run(false); lifecycleStatus=0;
    adapter.reset=true; run(false); adapter.reset=false;
    assert(parentCalls>0); run(true);
}
