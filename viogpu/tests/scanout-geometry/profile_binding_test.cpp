#include "viogpu_scanout_geometry_wire.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
using UINT=unsigned;
using ULONGLONG=unsigned long long;
using LONG64=long long;
using BOOLEAN=bool;
#define PAGED_CODE() ((void)0)
#define TRUE true
#define FALSE false
#define RtlZeroMemory(p,n) std::memset(p,0,n)
constexpr unsigned VIRTIO_GPU_F_NATIVE_SCANOUT_GEOMETRY=11;
bool virtio_is_feature_enabled(ULONGLONG flags,unsigned bit) { return (flags&(1ULL<<bit))!=0; }
LONG64 InterlockedCompareExchange64(volatile LONG64 *p,LONG64 value,LONG64 expected) {
    const auto old=*p; if(old==expected)*p=value; return old;
}
enum VIOGPU_HOST_CONTEXT_RESULT { VioGpuHostContextNotSubmitted,VioGpuHostContextConfirmed,
    VioGpuHostContextRejected,VioGpuHostContextUnknown };
struct Dod { bool reset{}; bool IsHardwareResetRequested() { return reset; } };
struct Queue {
    VIOGPU_SCANOUT_GEOMETRY geometry{1,64,1904,3040,3040,1904,1,0,83,31,{0,0}};
    VIOGPU_SCANOUT_PROFILE profile{1,64,7,1,1904,3040,3040,1904,23,29,{0,0}};
    ULONGLONG hostReset=83;
    unsigned queries{},binds{},mutation{};
    bool failQuery{};
    VIOGPU_HOST_CONTEXT_RESULT result=VioGpuHostContextConfirmed;
    volatile LONG64 *reset{};
    bool QueryScanoutGeometry(VIOGPU_SCANOUT_GEOMETRY_RESPONSE *r,bool negotiated) {
        assert(negotiated); ++queries; r->Geometry=geometry; return !failQuery;
    }
    bool QueryScanoutProfile(VIOGPU_SCANOUT_PROFILE_RESPONSE *r,bool negotiated) {
        assert(negotiated); ++queries; r->Profile=profile; r->HostResetGeneration=hostReset;
        if(mutation==1) ++*reset;
        return !failQuery;
    }
    VIOGPU_HOST_CONTEXT_RESULT BindScanoutGeometry(UINT id,const VIOGPU_SCANOUT_GEOMETRY *g,bool negotiated) {
        assert(negotiated && id==42 && VioGpuScanoutGeometryEqual(g,&geometry)); ++binds;
        if(mutation==2) ++profile.ProfileGeneration;
        if(mutation==3) ++geometry.ModeGeneration;
        if(mutation==4) ++*reset;
        return result;
    }
};
struct VioGpuAdapter {
    ULONGLONG m_u64GuestFeatures=1ULL<<11;
    volatile LONG64 m_NativeContextResetGeneration=112;
    Dod dod; Dod *m_pVioGpuDod=&dod;
    Queue m_CtrlQueue;
    VioGpuAdapter() { m_CtrlQueue.reset=&m_NativeContextResetGeneration; }
    bool SupportsNativeAhbPaging() const { return true; }
    BOOLEAN SupportsNativeScanoutGeometry() const;
    BOOLEAN QueryNativeScanoutState(VIOGPU_SCANOUT_GEOMETRY *,VIOGPU_SCANOUT_PROFILE *,ULONGLONG *);
    BOOLEAN NativeScanoutBindingCurrent(const VIOGPU_SCANOUT_BINDING *);
    VIOGPU_HOST_CONTEXT_RESULT BindNativeScanoutProfile(UINT,ULONGLONG,ULONGLONG,
        const VIOGPU_SCANOUT_GEOMETRY *,const VIOGPU_SCANOUT_PROFILE *,VIOGPU_SCANOUT_BINDING *);
};
// INSERT_PRODUCTION
int main()
{
    for(unsigned fault=0;fault<11;++fault) {
        VioGpuAdapter adapter;
        auto geometry=adapter.m_CtrlQueue.geometry;
        auto profile=adapter.m_CtrlQueue.profile;
        VIOGPU_SCANOUT_BINDING binding{};
        ULONGLONG local=112,key=93;
        switch(fault) {
        case 1: adapter.m_u64GuestFeatures=0; break;
        case 2: ++geometry.ModeGeneration; break;
        case 3: ++profile.ProfileGeneration; break;
        case 4: ++profile.EndpointGeneration; break;
        case 5: ++local; break;
        case 6: ++adapter.m_CtrlQueue.hostReset; break;
        case 7: adapter.dod.reset=true; break;
        case 8: adapter.m_CtrlQueue.failQuery=true; break;
        case 9: key=0; break;
        case 10: adapter.m_CtrlQueue.mutation=1; break;
        }
        auto result=adapter.BindNativeScanoutProfile(42,key,local,&geometry,&profile,&binding);
        assert(result==(fault ? VioGpuHostContextNotSubmitted : VioGpuHostContextConfirmed));
        assert(adapter.m_CtrlQueue.binds==(fault ? 0U : 1U));
        if(fault) assert(binding.ResourceId==0);
        else {
            assert(binding.ResourceId==42 && binding.ShareKey==93 && binding.LocalResetGeneration==112);
            assert(binding.EndpointGeneration==23 && binding.ProfileGeneration==29);
            assert(binding.Geometry.HostResetGeneration==83 && binding.Geometry.ModeGeneration==31);
            assert(adapter.NativeScanoutBindingCurrent(&binding));
            ++adapter.m_CtrlQueue.profile.ProfileGeneration;
            assert(!adapter.NativeScanoutBindingCurrent(&binding));
        }
    }
    for(unsigned mutation : {2U,3U,4U}) {
        VioGpuAdapter adapter;
        auto geometry=adapter.m_CtrlQueue.geometry; auto profile=adapter.m_CtrlQueue.profile;
        adapter.m_CtrlQueue.mutation=mutation;
        VIOGPU_SCANOUT_BINDING binding{};
        assert(adapter.BindNativeScanoutProfile(42,93,112,&geometry,&profile,&binding)==VioGpuHostContextUnknown);
        assert(adapter.m_CtrlQueue.binds==1 && !binding.ResourceId);
    }
    for(auto outcome : {VioGpuHostContextNotSubmitted,VioGpuHostContextRejected,VioGpuHostContextUnknown}) {
        VioGpuAdapter adapter; adapter.m_CtrlQueue.result=outcome;
        VIOGPU_SCANOUT_BINDING binding{};
        assert(adapter.BindNativeScanoutProfile(42,93,112,&adapter.m_CtrlQueue.geometry,
            &adapter.m_CtrlQueue.profile,&binding)==outcome && !binding.ResourceId);
    }
    VioGpuAdapter legacy;
    assert(!legacy.NativeScanoutBindingCurrent(nullptr));
    legacy.m_u64GuestFeatures=0;
    const auto priorQueries=legacy.m_CtrlQueue.queries;
    assert(legacy.NativeScanoutBindingCurrent(nullptr) && legacy.m_CtrlQueue.queries==priorQueries);
    legacy.m_u64GuestFeatures=1ULL<<11;
    legacy.m_CtrlQueue.geometry={1,64,3040,1904,3040,1904,0,0,83,0,{0,0}};
    legacy.m_CtrlQueue.profile={1,64,0,0,0,0,0,0,0,0,{0,0}};
    assert(legacy.NativeScanoutBindingCurrent(nullptr));
    ++legacy.m_CtrlQueue.geometry.ModeGeneration;
    assert(!legacy.NativeScanoutBindingCurrent(nullptr));
    puts("PASS actual adapter profile binding: distinct reset domains, tokens, revocation, failed/unknown publication");
}
