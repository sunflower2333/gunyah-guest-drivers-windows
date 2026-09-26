#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <set>
#include <vector>
using UINT=unsigned; using ULONG=unsigned; using ULONGLONG=unsigned long long;
using LONG64=long long; using SIZE_T=size_t; using NTSTATUS=int; using BOOLEAN=bool; using VOID=void;
#define PAGED_CODE() ((void)0)
#define RtlZeroMemory(p,n) std::memset(p,0,n)
#define VIOGPU_NATIVE_CONTEXT 1
#define DXGKDDI_INTERFACE_VERSION 0x5023
#define DXGKDDI_INTERFACE_VERSION_WDDM2_3 0x8001
#define DbgPrint(...) ((void)0)
constexpr UINT D3DDDIFMT_A8R8G8B8=21,D3DDDIFMT_X8R8G8B8=22,
               D3DDDIFMT_A8B8G8R8=32,D3DDDIFMT_X8B8G8R8=33;
constexpr UINT VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM=1,VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM=2,
               VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM=67,VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM=134;
constexpr UINT MAXUINT=~0U,PAGE_SIZE=4096,VIOGPU_MAX_BACKING_ENTRIES=1U<<18;
constexpr bool TRUE=true,FALSE=false;
constexpr int STATUS_SUCCESS=0,STATUS_DEVICE_NOT_READY=-1,STATUS_INVALID_PARAMETER=-2,STATUS_NO_MEMORY=-3;
constexpr unsigned PASSIVE_LEVEL=0;
unsigned KeGetCurrentIrql() { return 0; }
LONG64 InterlockedCompareExchange64(LONG64 *p,LONG64,LONG64) { return *p; }
unsigned allocationCount,failAllocation;
enum PoolTag { NonPagedPoolNx };
void *operator new(size_t size,PoolTag) noexcept {
    if(++allocationCount==failAllocation) return nullptr;
    return ::operator new(size,std::nothrow);
}
void operator delete(void *p,PoolTag) noexcept { ::operator delete(p); }
enum VIOGPU_HOST_CONTEXT_RESULT { VioGpuHostContextNotSubmitted,VioGpuHostContextConfirmed,
                                 VioGpuHostContextRejected,VioGpuHostContextUnknown };
enum VIOGPU_2D_RESOURCE_STATE { VioGpu2DResourceNone,VioGpu2DResourceCreated,
                              VioGpu2DResourceBackingAttached,VioGpu2DResourceUnknown };
struct GPU_MEM_ENTRY { ULONGLONG addr; unsigned length,padding; };
using PGPU_MEM_ENTRY=GPU_MEM_ENTRY*;
struct SG { unsigned NumberOfElements=1; struct { struct { ULONGLONG QuadPart=4096; } Address;
                                                unsigned Length=64U<<20; } Elements[1]; };
struct VioGpuMemSegment { unsigned size=64U<<20; SG sg; };
struct VioGpuObj {
    static unsigned alive; unsigned id{},size{}; VioGpuMemSegment *segment{};
    VioGpuObj() { ++alive; } ~VioGpuObj() { --alive; }
    bool Init(unsigned bytes,VioGpuMemSegment *s) {
        if(bytes>s->size) return false;
        size=(bytes+4095)&~4095U; segment=s; return true;
    }
    UINT GetId() { return id; } void SetId(UINT v) { id=v; }
    UINT GetSize() { return size; } SG *GetSGList() { return &segment->sg; }
    void *GetVirtualAddress() { return segment; }
};
unsigned VioGpuObj::alive;
struct VIOGPU_NATIVE_FRAMEBUFFER {
    VioGpuObj *Object; VIOGPU_2D_RESOURCE_STATE State; ULONGLONG ResetGeneration;
    UINT Width,Height; VIOGPU_NATIVE_FRAMEBUFFER *Next;
};
struct VIDEO_MODE_INFORMATION { unsigned ModeIndex=8,VisScreenWidth=3040,VisScreenHeight=1904,ScreenStride=12160; };
using PVIDEO_MODE_INFORMATION=VIDEO_MODE_INFORMATION*;
struct CURRENT_MODE { struct { unsigned Width=3040,Height=1904,ColorFormat=D3DDDIFMT_A8R8G8B8; } DispInfo;
                      void *FrameBuffer{}; struct { bool FrameBufferIsActive{}; } Flags; };
struct Dod { bool reset{}; unsigned requests{};
    bool IsHardwareResetRequested() { return reset; }
    void RequestHardwareResetAtAnyIrql() { reset=true; ++requests; }
};
struct BufferPool {
    bool fail{}; unsigned live{};
    void *AllocateMemory(size_t size) { if(fail)return nullptr; ++live; return ::operator new(size); }
    void FreeMemory(void *p) { assert(live); --live; ::operator delete(p); }
};
struct Queue {
    bool healthy=true; unsigned transfers{},flushes{};
    VIOGPU_HOST_CONTEXT_RESULT transferResult=VioGpuHostContextConfirmed,flushResult=VioGpuHostContextConfirmed;
    auto TransferToHost2DSynchronous(UINT,UINT,UINT,UINT,UINT,UINT) {
        ++transfers; if(transferResult==VioGpuHostContextUnknown)healthy=false; return transferResult;
    }
    auto FlushResourceSynchronous(UINT,UINT,UINT,UINT,UINT) {
        ++flushes; if(flushResult==VioGpuHostContextUnknown)healthy=false; return flushResult;
    }
};
struct VioGpuAdapter {
    Dod dod; Dod *m_pVioGpuDod=&dod; Queue m_CtrlQueue; BufferPool m_GpuBuf;
    VioGpuMemSegment m_FrameSegment; VIDEO_MODE_INFORMATION m_ModeInfo[1];
    VIOGPU_NATIVE_FRAMEBUFFER *m_FrameBufferOwner{},*m_RetiredFrameBuffers{};
    VioGpuObj *m_pFrameBuf{}; UINT m_FrameBufWidth{},m_FrameBufHeight{};
    LONG64 m_NativeContextResetGeneration=7;
    ULONGLONG retired{}; unsigned nextId=1,active{},binds{},creates{},unrefs{},releasedIds{};
    bool failId{},resetDuringBind{}; std::set<unsigned> ids,host;
    VIOGPU_HOST_CONTEXT_RESULT createResult=VioGpuHostContextConfirmed,
        bindResult=VioGpuHostContextConfirmed,unrefResult=VioGpuHostContextConfirmed;
    unsigned GetModeCount() { return 1; }
    unsigned Allocate2DResourceId() { if(failId)return 0; unsigned id=nextId++; assert(ids.insert(id).second); return id; }
    bool Release2DResourceId(unsigned id) { assert(!host.count(id)); assert(ids.erase(id)==1); ++releasedIds; return true; }
    VIOGPU_HOST_CONTEXT_RESULT Create2DResourceBacking(UINT id,UINT format,UINT width,UINT height,SIZE_T size,
        const GPU_MEM_ENTRY *entries,UINT count,VIOGPU_2D_RESOURCE_STATE *state,ULONGLONG *reset) {
        ++creates; assert(format==VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM && width==3040 && height==1904 && size>=width*height*4);
        assert(entries && count==1 && entries[0].padding==0);
        if(createResult==VioGpuHostContextConfirmed || createResult==VioGpuHostContextUnknown) {
            assert(host.insert(id).second); *reset=7;
            *state=createResult==VioGpuHostContextConfirmed?VioGpu2DResourceBackingAttached:VioGpu2DResourceUnknown;
        }
        if(createResult==VioGpuHostContextUnknown) m_CtrlQueue.healthy=false;
        return createResult;
    }
    VIOGPU_HOST_CONTEXT_RESULT Destroy2DResource(UINT id,VIOGPU_2D_RESOURCE_STATE *state,
        ULONGLONG *reset,BOOLEAN *released,BOOLEAN retain) {
        *released=false; assert(retain);
        if(*reset && *reset<=retired) { *state=VioGpu2DResourceNone; *reset=0; host.erase(id); }
        if(*state==VioGpu2DResourceNone) { *released=true; return VioGpuHostContextConfirmed; }
        if(*reset!=static_cast<ULONGLONG>(m_NativeContextResetGeneration)) {
            *state=VioGpu2DResourceUnknown; return VioGpuHostContextUnknown;
        }
        if(!m_CtrlQueue.healthy || *state==VioGpu2DResourceUnknown) return VioGpuHostContextUnknown;
        ++unrefs;
        if(unrefResult==VioGpuHostContextConfirmed) {
            assert(active!=id); assert(host.erase(id)==1); *state=VioGpu2DResourceNone; *reset=0; *released=true;
        }
        return unrefResult;
    }
    VIOGPU_HOST_CONTEXT_RESULT Set2DScanout(UINT scanout,UINT resource,UINT width,UINT height,UINT *previous) {
        assert(scanout==0 && host.count(resource) && width==3040 && height==1904); ++binds; *previous=active;
        if(bindResult==VioGpuHostContextConfirmed) active=resource;
        if(resetDuringBind) m_NativeContextResetGeneration=8;
        if(bindResult==VioGpuHostContextUnknown) { m_CtrlQueue.healthy=false; dod.reset=true; }
        return bindResult;
    }
    BOOLEAN ReleaseFrameBufferOwner(VIOGPU_NATIVE_FRAMEBUFFER*);
    VOID RetireFrameBufferMode(VIOGPU_NATIVE_FRAMEBUFFER*);
    BOOLEAN CollectRetiredFrameBuffers();
    NTSTATUS PrepareFrameBufferMode(ULONG,const CURRENT_MODE*,VIOGPU_NATIVE_FRAMEBUFFER**);
    NTSTATUS CommitFrameBufferMode(VIOGPU_NATIVE_FRAMEBUFFER*,CURRENT_MODE*);
    void shutdown() {
        retired=7; active=0; host.clear(); m_CtrlQueue.healthy=false;
        RetireFrameBufferMode(m_FrameBufferOwner); m_FrameBufferOwner=nullptr; m_pFrameBuf=nullptr;
        assert(CollectRetiredFrameBuffers() && ids.empty() && !m_GpuBuf.live);
    }
};
// INSERT_PRODUCTION
static VIOGPU_NATIVE_FRAMEBUFFER *prepare(VioGpuAdapter &a,CURRENT_MODE &mode) {
    VIOGPU_NATIVE_FRAMEBUFFER *p=nullptr;
    assert(a.PrepareFrameBufferMode(8,&mode,&p)==0 && p);
    return p;
}
int main() {
    {
        VioGpuAdapter a; CURRENT_MODE mode;
        auto first=prepare(a,mode); assert(!a.active && !a.m_pFrameBuf && !mode.Flags.FrameBufferIsActive);
        assert(a.CommitFrameBufferMode(first,&mode)==0); const auto oldId=first->Object->GetId();
        auto next=prepare(a,mode);
        assert(a.active==oldId && a.host.count(oldId) && a.m_FrameBufferOwner==first);
        a.bindResult=VioGpuHostContextRejected;
        assert(a.CommitFrameBufferMode(next,&mode)==-1 && a.m_FrameBufferOwner==first);
        a.RetireFrameBufferMode(next); assert(a.ids.size()==1 && a.host.count(oldId));
        a.bindResult=VioGpuHostContextConfirmed;
        next=prepare(a,mode); a.unrefResult=VioGpuHostContextRejected;
        assert(a.CommitFrameBufferMode(next,&mode)==0 && a.m_FrameBufferOwner==next);
        assert(a.m_RetiredFrameBuffers==first && a.ids.size()==2 && a.host.size()==2);
        VIOGPU_NATIVE_FRAMEBUFFER *refused=nullptr;
        assert(a.PrepareFrameBufferMode(8,&mode,&refused)==-1 && !refused); // Bounded pending ownership.
        a.unrefResult=VioGpuHostContextConfirmed;
        assert(a.CollectRetiredFrameBuffers() && !a.host.count(oldId)); a.shutdown();
    }
    for(unsigned failure=0;failure!=11;++failure) {
        VioGpuAdapter a; CURRENT_MODE mode; auto first=prepare(a,mode);
        assert(a.CommitFrameBufferMode(first,&mode)==0); auto prior=mode; unsigned oldId=a.active;
        if(failure==0) failAllocation=allocationCount+1;
        if(failure==1) failAllocation=allocationCount+2;
        if(failure==2) a.m_FrameSegment.size=4096;
        if(failure==3) a.m_GpuBuf.fail=true;
        if(failure==4) a.failId=true;
        if(failure==5) a.createResult=VioGpuHostContextRejected;
        if(failure==6) a.createResult=VioGpuHostContextUnknown;
        if(failure==7) a.m_CtrlQueue.transferResult=VioGpuHostContextRejected;
        if(failure==8) a.m_CtrlQueue.transferResult=VioGpuHostContextUnknown;
        if(failure==9) a.m_CtrlQueue.flushResult=VioGpuHostContextRejected;
        if(failure==10) a.m_CtrlQueue.flushResult=VioGpuHostContextUnknown;
        VIOGPU_NATIVE_FRAMEBUFFER *out=nullptr;
        assert(a.PrepareFrameBufferMode(8,&mode,&out)!=0 && !out && a.binds==1);
        assert(a.active==oldId && a.m_FrameBufferOwner==first && a.host.count(oldId));
        assert(std::memcmp(&mode,&prior,sizeof(mode))==0 && !a.m_GpuBuf.live);
        if(failure==6 || failure==8 || failure==10)
            assert(a.m_RetiredFrameBuffers && a.ids.size()==2 && a.dod.reset);
        a.shutdown(); failAllocation=0;
    }
    {
        VioGpuAdapter a; CURRENT_MODE mode; auto first=prepare(a,mode);
        assert(a.CommitFrameBufferMode(first,&mode)==0);
        auto candidate=prepare(a,mode); a.bindResult=VioGpuHostContextUnknown;
        assert(a.CommitFrameBufferMode(candidate,&mode)==-1 && a.m_FrameBufferOwner==first);
        a.RetireFrameBufferMode(candidate);
        assert(a.ids.size()==2 && a.m_RetiredFrameBuffers==candidate); a.shutdown();
    }
    {
        VioGpuAdapter a; CURRENT_MODE mode; auto first=prepare(a,mode);
        assert(a.CommitFrameBufferMode(first,&mode)==0);
        auto candidate=prepare(a,mode); a.resetDuringBind=true;
        assert(a.CommitFrameBufferMode(candidate,&mode)==-1 && a.m_FrameBufferOwner==first && a.dod.reset);
        a.RetireFrameBufferMode(candidate);
        assert(a.ids.size()==2 && a.m_RetiredFrameBuffers==candidate); a.shutdown();
    }
    assert(VioGpuObj::alive==0);
    puts("PASS actual framebuffer prepare/commit/retire, allocation/host failures and confirmed reset ownership");
}
