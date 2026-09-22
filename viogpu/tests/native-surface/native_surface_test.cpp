#include <cassert>
#include <cstddef>
#include <cstring>
#include <new>
#include "viogpu_native_surface_policy.h"
#include "viogpu_native_ahb_access.h"

#define _In_
#define _Out_
#define _Inout_
#define PAGED_CODE() ((void)0)
#define __try try
#define __except(x) catch (...)
#define NT_SUCCESS(x) ((x) == 0)
#define RtlZeroMemory(p, n) std::memset(p, 0, n)
#define RtlCopyMemory(p, q, n) std::memcpy(p, q, n)
#define TRUE true
#define FALSE false
using UINT = unsigned int;
using SIZE_T = std::size_t;
using ULONG = unsigned int;
using LONG = int;
using KEVENT = bool;
constexpr int NotificationEvent = 0;
void KeInitializeEvent(KEVENT *event, int, bool value) { *event=value; }
LONG InterlockedCompareExchange(volatile LONG *p, LONG value, LONG expected) {
    LONG old=*p; if(old==expected) *p=value; return old;
}
using ULONGLONG = unsigned long long;
constexpr ULONGLONG VIOGPU_WDDM_HOST_SURFACE_BUDGET=256ULL*1024*1024;
using BOOLEAN = bool;
using VOID = void;
using NTSTATUS = int;
using PEPROCESS = int *;
constexpr UINT VIOGPU_NATIVE_RESOURCE_ID_START = 0x80000000U;
constexpr UINT MAXUINT = ~0U;
constexpr int STATUS_SUCCESS = 0, STATUS_INVALID_PARAMETER = -1, STATUS_INVALID_USER_BUFFER = -2,
    STATUS_DEVICE_NOT_READY = -3, STATUS_INVALID_HANDLE = -4, STATUS_INSUFFICIENT_RESOURCES = -5, STATUS_NO_MEMORY = -6,
    STATUS_GRAPHICS_ALLOCATION_BUSY = -7;
constexpr int NonPagedPoolNx = 0;
void *operator new(std::size_t size, int) { return ::operator new(size); }
enum VIOGPU_HOST_CONTEXT_RESULT { VioGpuHostContextNotSubmitted, VioGpuHostContextConfirmed,
    VioGpuHostContextRejected, VioGpuHostContextUnknown };
enum VIOGPU_2D_RESOURCE_STATE { VioGpu2DResourceNone, VioGpu2DResourceNativeAhbBackingAttached,
    VioGpu2DResourceUnknown };
struct LIST_ENTRY { LIST_ENTRY *Flink, *Blink; };
using PLIST_ENTRY = LIST_ENTRY *;
#define CONTAINING_RECORD(p, t, f) reinterpret_cast<t *>(reinterpret_cast<char *>(p) - offsetof(t, f))
void InitializeListHead(LIST_ENTRY *p) { p->Flink = p->Blink = p; }
void InsertTailList(LIST_ENTRY *h, LIST_ENTRY *p) { p->Blink=h->Blink; p->Flink=h; h->Blink->Flink=p; h->Blink=p; }
void RemoveEntryList(LIST_ENTRY *p) { p->Blink->Flink=p->Flink; p->Flink->Blink=p->Blink; }
static int owner, stranger, objectReferences;
static PEPROCESS currentProcess = &owner;
PEPROCESS PsGetCurrentProcess() { return currentProcess; }
void ObReferenceObject(PEPROCESS) { ++objectReferences; }
void ObDereferenceObject(PEPROCESS) { --objectReferences; }
struct GPU_CAPSET_DRM {};
struct VIOGPU_PRIMARY_SCANOUT_LAYOUT { UINT Width, Height, Format, Stride; ULONGLONG BackingSize; };
struct DXGKARG_ESCAPE { void *hDevice{}, *hContext{}; struct { UINT Value{}; } Flags;
    void *pPrivateDriverData{}; UINT PrivateDriverDataSize{}; };
struct VIOGPU_WDDM_CONTEXT {};
class VioGpuDod;
struct VIOGPU_NATIVE_CONTEXT_SNAPSHOT { VioGpuDod *Adapter; UINT ContextId; ULONGLONG ResetGeneration; };
struct VIOGPU_WDDM_ALLOCATION { BOOLEAN HostSurface{}; VioGpuDod *Adapter{}; ULONGLONG ShareKey{}; UINT ResourceId{}; };
class VioGpuDod {
public:
    ULONGLONG generation=7;
    UINT nextId=1, created=0, destroyed=0, imported=0, detached=0;
    bool enabled=true, busy=false, failRelease=false, failImport=false;
    bool IsDriverActive() { return true; }
    bool IsNativeAhbScanoutEnabled() { return enabled; }
    bool SupportsNativeAhbPaging() { return enabled; }
    bool QueryNativeContextReadiness(GPU_CAPSET_DRM *, void *, void *, ULONGLONG *g) { *g=generation; return true; }
    UINT Allocate2DResourceId() { return nextId++; }
    bool Release2DResourceId(UINT) { return true; }
    bool AcquireNativeSubmissionOperation() { return true; }
    void ReleaseNativeSubmissionOperation() {}
    VIOGPU_HOST_CONTEXT_RESULT Create2DResourceBacking(UINT, UINT f, UINT w, UINT h, int,
        void *, int, VIOGPU_2D_RESOURCE_STATE *state, ULONGLONG *g, bool, bool native,
        VIOGPU_PRIMARY_SCANOUT_LAYOUT *layout) {
        assert(native); ++created; *state=VioGpu2DResourceNativeAhbBackingAttached; *g=generation;
        *layout={w,h,f,512,8192}; return VioGpuHostContextConfirmed;
    }
    VIOGPU_HOST_CONTEXT_RESULT Destroy2DResource(UINT, VIOGPU_2D_RESOURCE_STATE *state,
        ULONGLONG *g, BOOLEAN *released, bool retain) {
        assert(retain); *released=false;
        if(busy) return VioGpuHostContextRejected;
        ++destroyed; *state=VioGpu2DResourceNone; *g=0; *released=true; return VioGpuHostContextConfirmed;
    }
    bool Reconcile2DResourceAfterReset(VIOGPU_2D_RESOURCE_STATE *s, ULONGLONG *g, BOOLEAN *retired) {
        *retired=*g!=generation; if(*retired) { *s=VioGpu2DResourceNone; *g=0; } return true;
    }
    VIOGPU_HOST_CONTEXT_RESULT ImportNativeSharedResource(const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *, UINT, ULONGLONG, bool native) {
        assert(native); ++imported; return failImport ? VioGpuHostContextUnknown : VioGpuHostContextConfirmed;
    }
    VIOGPU_HOST_CONTEXT_RESULT ReleaseNativeSharedResource(const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *, UINT, bool native) {
        assert(native); if(failRelease) return VioGpuHostContextUnknown; ++detached; return VioGpuHostContextConfirmed;
    }
};
bool IsCurrentAbiHeader(const VIOGPU_WDDM_ABI_HEADER *h, UINT n) {
    return h->Magic==VIOGPU_WDDM_ABI_MAGIC && h->Version==0 && h->Reserved==0 && h->Size==n;
}
UINT FromPrivateFormat(UINT f) { return f; }
bool ResolveStandard2DFormat(UINT f, UINT *v) { *v=f==3 ? 67 : f; return true; }
void FindNativeAllocationRangeByResourceId(VIOGPU_WDDM_CONTEXT *, UINT, UINT, ULONGLONG *i, ULONGLONG *s) { *i=0; *s=0; }
// INSERT_STRUCTS
static LIST_ENTRY g_VioGpuNativeShares, g_VioGpuNativeImports;
static bool initialized;
bool AcquireNativeShareRegistry(bool) {
    if(!initialized) { initialized=true; InitializeListHead(&g_VioGpuNativeShares); InitializeListHead(&g_VioGpuNativeImports); }
    return true;
}
void ReleaseNativeShareRegistry() {}
ULONGLONG NewNativeShareKeyLocked(VioGpuDod *) { static ULONGLONG key=10; return ++key; }
// INSERT_PRODUCTION

static VIOGPU_WDDM_NATIVE_SURFACE allocate(VioGpuDod &adapter) {
    VIOGPU_WDDM_NATIVE_SURFACE s{};
    s.Header={VIOGPU_WDDM_ABI_MAGIC,0,sizeof(s),0}; s.Opcode=8;
    s.Width=64; s.Height=16; s.Fourcc=VIOGPU_NATIVE_AHB_FOURCC_AR24;
    DXGKARG_ESCAPE e{}; e.pPrivateDriverData=&s; e.PrivateDriverDataSize=sizeof(s);
    assert(HandleNativeSurfaceEscape(&adapter,&e)==STATUS_SUCCESS);
    assert(s.ContextId==0 && s.ResetGeneration==adapter.generation && s.Size==8192 && s.Stride==512);
    return s;
}
static int free_surface(VioGpuDod &adapter, VIOGPU_WDDM_NATIVE_SURFACE s) {
    s.Opcode=9; s.ExpectedResetGeneration=s.ResetGeneration;
    DXGKARG_ESCAPE e{}; e.pPrivateDriverData=&s; e.PrivateDriverDataSize=sizeof(s);
    return HandleNativeSurfaceEscape(&adapter,&e);
}
int main() {
    static_assert(sizeof(VIOGPU_WDDM_NATIVE_SURFACE)==128);
    static_assert(offsetof(VIOGPU_WDDM_NATIVE_SURFACE,ResourceId)==72);
    static_assert(offsetof(VIOGPU_WDDM_NATIVE_SURFACE,Reserved)==104);
    VioGpuDod adapter;
    VIOGPU_WDDM_NATIVE_SURFACE malformed{};
    malformed.Header={VIOGPU_WDDM_ABI_MAGIC,0,sizeof(malformed),0}; malformed.Opcode=8;
    malformed.Width=64; malformed.Height=16; malformed.Fourcc=VIOGPU_NATIVE_AHB_FOURCC_AR24;
    DXGKARG_ESCAPE invalid{}; invalid.pPrivateDriverData=&malformed; invalid.PrivateDriverDataSize=sizeof(malformed);
    invalid.hContext=&adapter;
    assert(HandleNativeSurfaceEscape(&adapter,&invalid)==STATUS_INVALID_PARAMETER);
    invalid.hContext=nullptr; malformed.ContextId=1;
    assert(HandleNativeSurfaceEscape(&adapter,&invalid)==STATUS_INVALID_PARAMETER);
    malformed.ContextId=0; malformed.ExpectedResetGeneration=7;
    assert(HandleNativeSurfaceEscape(&adapter,&invalid)==STATUS_INVALID_PARAMETER);
    malformed.ExpectedResetGeneration=0; malformed.Stride=256;
    assert(HandleNativeSurfaceEscape(&adapter,&invalid)==STATUS_INVALID_PARAMETER);
    malformed.Stride=0; malformed.Fourcc=VIOGPU_NATIVE_AHB_FOURCC_XR24;
    assert(HandleNativeSurfaceEscape(&adapter,&invalid)==STATUS_INVALID_PARAMETER);
    malformed.Fourcc=VIOGPU_NATIVE_AHB_FOURCC_AR24; malformed.Width=16385;
    assert(HandleNativeSurfaceEscape(&adapter,&invalid)==STATUS_INVALID_PARAMETER);
    assert(adapter.created==0);
    auto s=allocate(adapter);
    auto forged=s; ++forged.Stride;
    assert(free_surface(adapter,forged)==STATUS_INVALID_HANDLE);
    currentProcess=&stranger;
    assert(free_surface(adapter,s)==STATUS_INVALID_HANDLE);
    currentProcess=&owner;
    VIOGPU_WDDM_CONTEXT context{};
    VIOGPU_NATIVE_CONTEXT_SNAPSHOT snap{&adapter,42,adapter.generation};
    VIOGPU_WDDM_NATIVE_SHARE req{}; req.ShareKey=s.ShareKey; req.Size=s.Size; req.Iova=0x10000;
    ULONG stage{},host{},ownerContext{},rid{}; ULONGLONG size{};
    ++snap.ResetGeneration;
    assert(ImportNativeShareLocked(&adapter,&context,&snap,&req,&stage,&size,&host,&ownerContext,&rid)!=STATUS_SUCCESS);
    --snap.ResetGeneration;
    assert(ImportNativeShareLocked(&adapter,&context,&snap,&req,&stage,&size,&host,&ownerContext,&rid)==STATUS_SUCCESS);
    assert(rid==s.ResourceId && ownerContext==0 && adapter.created==1);
    assert(free_surface(adapter,s)==STATUS_SUCCESS);
    assert(adapter.destroyed==0 && objectReferences==1);
    adapter.failRelease=true;
    assert(ReleaseNativeShareLocked(&adapter,&context,&snap,&req,&stage)!=STATUS_SUCCESS);
    assert(adapter.destroyed==0 && FindNativeShareByKeyLocked(&adapter,s.ShareKey)->ImportReferences==1);
    adapter.failRelease=false;
    assert(ReleaseNativeShareLocked(&adapter,&context,&snap,&req,&stage)==STATUS_SUCCESS);
    assert(adapter.destroyed==1 && objectReferences==0);
    assert(free_surface(adapter,s)==STATUS_INVALID_HANDLE);

    s=allocate(adapter);
    VIOGPU_WDDM_RESOURCE_SHARE resource{}; resource.ShareKey=s.ShareKey; resource.Stride=s.Stride;
    VIOGPU_WDDM_ALLOCATION_INFO info{}; info.Size=s.Size; info.Width=s.Width; info.Height=s.Height;
    info.Pitch=s.Stride; info.Format=1;
    UINT id{}; ULONGLONG generation{};
    ++info.Pitch;
    assert(ReferenceHostSurfaceAllocation(&adapter,&resource,&info,&id,&generation)!=STATUS_SUCCESS);
    --info.Pitch;
    assert(ReferenceHostSurfaceAllocation(&adapter,&resource,&info,&id,&generation)==STATUS_SUCCESS);
    assert(id==s.ResourceId && generation==s.ResetGeneration && adapter.created==2);
    assert(ReferenceHostSurfaceAllocation(&adapter,&resource,&info,&id,&generation)!=STATUS_SUCCESS);
    assert(free_surface(adapter,s)==STATUS_SUCCESS);
    assert(adapter.destroyed==1);
    VIOGPU_WDDM_ALLOCATION allocation{true,&adapter,s.ShareKey,s.ResourceId};
    adapter.busy=true; ReleaseHostSurfaceAllocation(&allocation);
    assert(adapter.destroyed==1 && objectReferences==1 && allocation.ResourceId==0);
    adapter.busy=false; CollectNativeSurfacesLocked(&adapter);
    assert(adapter.destroyed==2 && objectReferences==0);

    s=allocate(adapter); req.ShareKey=s.ShareKey; adapter.failImport=true;
    assert(ImportNativeShareLocked(&adapter,&context,&snap,&req,&stage,&size,&host,&ownerContext,&rid)!=STATUS_SUCCESS);
    assert(free_surface(adapter,s)==STATUS_SUCCESS && adapter.destroyed==2);
    RemoveNativeImportsForContext(&context); // Host context already confirmed destroyed.
    assert(adapter.destroyed==3 && objectReferences==0);
    s=allocate(adapter);
    VioGpuWddmRetireNativeShares(&adapter);
    assert(objectReferences==0 && FindNativeShareByKeyLocked(&adapter,s.ShareKey)==nullptr);
}
