#include "viogpu_scanout_geometry_wire.h"
#include "viogpu_native_scanout_mode.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
using UINT=unsigned;
using ULONGLONG=unsigned long long;
using LONG64=long long;
using BOOLEAN=bool;
using VOID=void;
#define PAGED_CODE() ((void)0)
#define TRUE true
#define FALSE false
#define RtlZeroMemory(p,n) std::memset(p,0,n)
constexpr unsigned VIRTIO_GPU_F_NATIVE_SCANOUT_GEOMETRY=11;
constexpr unsigned VIRTIO_GPU_RESP_OK_SCANOUT_PROFILE=0xd222;
constexpr int Executive=0,KernelMode=0,STATUS_SUCCESS=0;
struct Mutex { bool owned{},fail{}; };
int KeWaitForSingleObject(Mutex *m,int,int,bool,void *) { if(m->fail)return -1; assert(!m->owned); m->owned=true; return 0; }
void KeReleaseMutex(Mutex *m,bool) { assert(m->owned); m->owned=false; }
bool virtio_is_feature_enabled(ULONGLONG flags,unsigned bit) { return (flags&(1ULL<<bit))!=0; }
LONG64 InterlockedCompareExchange64(volatile LONG64 *p,LONG64 value,LONG64 expected) {
    const auto old=*p; if(old==expected)*p=value; return old;
}
enum VIOGPU_HOST_CONTEXT_RESULT { VioGpuHostContextNotSubmitted,VioGpuHostContextConfirmed,
    VioGpuHostContextRejected,VioGpuHostContextUnknown };
struct Dod {
    bool reset{};
    bool IsHardwareResetRequested() { return reset; }
    void RequestHardwareResetAtAnyIrql() { reset=true; }
};
struct Queue {
    VIOGPU_SCANOUT_GEOMETRY geometry{1,64,3040,1904,3040,1904,0,0,83,0,{0,0}};
    VIOGPU_SCANOUT_PROFILE profile{1,64,7,3,1904,3040,3040,1904,23,29,{0,0}};
    ULONGLONG hostReset=83;
    unsigned queries{},configures{},afterCommitQueries{};
    bool failQuery{},failAfterCommit{},mutateAfterCommit{};
    VIOGPU_HOST_CONTEXT_RESULT result=VioGpuHostContextConfirmed;
    bool QueryScanoutGeometry(VIOGPU_SCANOUT_GEOMETRY_RESPONSE *r,bool negotiated) {
        assert(negotiated); ++queries; if(configures)++afterCommitQueries;
        r->Geometry=geometry; return !failQuery && !(configures && failAfterCommit);
    }
    bool QueryScanoutProfile(VIOGPU_SCANOUT_PROFILE_RESPONSE *r,bool negotiated) {
        assert(negotiated); ++queries; if(configures)++afterCommitQueries;
        r->Profile=profile; r->HostResetGeneration=hostReset;
        return !failQuery && !(configures && failAfterCommit);
    }
    VIOGPU_HOST_CONTEXT_RESULT ConfigureScanoutProfile(const VIOGPU_SCANOUT_GEOMETRY *g,
        const VIOGPU_SCANOUT_PROFILE_RESPONSE *p,bool negotiated) {
        assert(negotiated && VioGpuScanoutProfileResponseValid(p,sizeof(*p)));
        assert(VioGpuScanoutProfileEqual(&profile,&p->Profile));
        assert(g->ModeGeneration>geometry.ModeGeneration && g->HostResetGeneration==hostReset);
        ++configures;
        if(result==VioGpuHostContextConfirmed || result==VioGpuHostContextUnknown)geometry=*g;
        if(mutateAfterCommit)++profile.ProfileGeneration;
        return result;
    }
};
struct VioGpuAdapter {
    ULONGLONG m_u64GuestFeatures=1ULL<<11;
    volatile LONG64 m_NativeContextResetGeneration=112;
    Dod dod; Dod *m_pVioGpuDod=&dod;
    Queue m_CtrlQueue;
    Mutex m_NativeScanoutModeMutex;
    VIOGPU_NATIVE_SCANOUT_MODE m_NativeScanoutCandidate{},m_NativeScanoutCommitted{};
    ULONGLONG m_NativeScanoutReservedHostReset{},m_NativeScanoutReservedMode{};
    VIOGPU_SCANOUT_PROFILE m_NativeScanoutObservedProfile{};
    ULONGLONG m_NativeScanoutObservedHostReset{},m_NativeScanoutObservedLocalReset{},m_NativeScanoutObservation{};
    BOOLEAN m_NativeScanoutObservationValid{},m_NativeScanoutReconcilePending{};
    bool acquire=true; unsigned operations{};
    bool AcquireNativeSubmitOperation() { if(!acquire)return false; ++operations; return true; }
    void ReleaseNativeSubmitOperation() { assert(operations); --operations; }
    bool SupportsNativeAhbPaging() const { return true; }
    BOOLEAN SupportsNativeScanoutGeometry() const;
    BOOLEAN QueryNativeScanoutState(VIOGPU_SCANOUT_GEOMETRY *,VIOGPU_SCANOUT_PROFILE *,ULONGLONG *);
    BOOLEAN ReserveNativeScanoutMode(VIOGPU_NATIVE_SCANOUT_MODE *);
    BOOLEAN QueryReservedNativeScanoutMode(VIOGPU_NATIVE_SCANOUT_MODE *,BOOLEAN *);
    BOOLEAN NativeScanoutModeEligible(const VIOGPU_NATIVE_SCANOUT_MODE *,BOOLEAN);
    VIOGPU_HOST_CONTEXT_RESULT CommitNativeScanoutMode(const VIOGPU_NATIVE_SCANOUT_MODE *);
    VOID ObserveNativeScanoutProfile();
};
// INSERT_PRODUCTION
static void drained(const VioGpuAdapter &a) { assert(!a.operations && !a.m_NativeScanoutModeMutex.owned); }
int main()
{
    VioGpuAdapter a;
    VIOGPU_NATIVE_SCANOUT_MODE first{},again{};
    bool committed=true;
    assert(!a.QueryReservedNativeScanoutMode(&again,&committed) && !committed);
    assert(!a.m_NativeScanoutReservedMode);
    assert(a.ReserveNativeScanoutMode(&first));
    assert(a.QueryReservedNativeScanoutMode(&again,&committed) && !committed);
    assert(VioGpuNativeScanoutModeEqual(&first,&again) && a.m_NativeScanoutReservedMode==1);
    assert(first.Geometry.ModeGeneration==1 && first.Geometry.ContentRotationCw==3);
    assert(first.Geometry.StorageWidth==1904 && first.Geometry.LogicalWidth==3040);
    assert(first.LocalResetGeneration==112 && first.Geometry.HostResetGeneration==83);
    assert(!a.m_CtrlQueue.configures && !VioGpuNativeScanoutModeValid(&a.m_NativeScanoutCommitted));
    assert(a.NativeScanoutModeEligible(&first,false) && !a.NativeScanoutModeEligible(&first,true));
    assert(a.ReserveNativeScanoutMode(&again) && VioGpuNativeScanoutModeEqual(&first,&again));
    assert(a.CommitNativeScanoutMode(&first)==VioGpuHostContextConfirmed);
    assert(a.m_CtrlQueue.configures==1 && a.m_CtrlQueue.afterCommitQueries==0);
    assert(VioGpuNativeScanoutModeEqual(&first,&a.m_NativeScanoutCommitted));
    assert(!VioGpuNativeScanoutModeValid(&a.m_NativeScanoutCandidate));
    assert(a.NativeScanoutModeEligible(&first,true));
    assert(a.ReserveNativeScanoutMode(&again) && VioGpuNativeScanoutModeEqual(&first,&again));
    assert(a.CommitNativeScanoutMode(&again)==VioGpuHostContextConfirmed && a.m_CtrlQueue.configures==1);
    assert(a.QueryReservedNativeScanoutMode(&again,&committed) && committed);
    assert(VioGpuNativeScanoutModeEqual(&first,&again));
    drained(a);

    {
        VioGpuAdapter q;
        assert(q.ReserveNativeScanoutMode(&again));
        q.m_CtrlQueue.geometry=again.Geometry; // Host active, local commit ownership absent.
        assert(!q.QueryReservedNativeScanoutMode(&again,&committed) && !committed);
        assert(!again.LocalResetGeneration && q.m_NativeScanoutReservedMode==1);
    }

    for(unsigned fault=0;fault<12;++fault) {
        VioGpuAdapter q; VIOGPU_NATIVE_SCANOUT_MODE candidate{};
        assert(q.ReserveNativeScanoutMode(&candidate));
        switch(fault) {
        case 0: ++candidate.LocalResetGeneration; break;
        case 1: ++candidate.Geometry.HostResetGeneration; break;
        case 2: ++candidate.Geometry.ModeGeneration; break;
        case 3: ++candidate.Profile.ProfileGeneration; break;
        case 4: ++q.m_CtrlQueue.profile.ProfileGeneration; break;
        case 5: ++q.m_CtrlQueue.profile.EndpointGeneration; break;
        case 6: q.m_CtrlQueue.geometry.ModeGeneration=candidate.Geometry.ModeGeneration; break;
        case 7: q.m_CtrlQueue.failQuery=true; break;
        case 8: q.m_u64GuestFeatures=0; break;
        case 9: q.dod.reset=true; break;
        case 10: ++q.m_NativeContextResetGeneration; break;
        case 11: ++q.m_CtrlQueue.geometry.HostResetGeneration; ++q.m_CtrlQueue.hostReset; break;
        }
        assert(q.CommitNativeScanoutMode(&candidate)==VioGpuHostContextNotSubmitted);
        assert(!q.m_CtrlQueue.configures && !VioGpuNativeScanoutModeValid(&q.m_NativeScanoutCommitted));
        drained(q);
    }
    for(auto result : {VioGpuHostContextNotSubmitted,VioGpuHostContextRejected,VioGpuHostContextUnknown}) {
        VioGpuAdapter q; VIOGPU_NATIVE_SCANOUT_MODE candidate{};
        assert(q.ReserveNativeScanoutMode(&candidate)); q.m_CtrlQueue.result=result;
        assert(q.CommitNativeScanoutMode(&candidate)==result);
        assert(!VioGpuNativeScanoutModeValid(&q.m_NativeScanoutCommitted));
        assert(q.dod.reset==(result==VioGpuHostContextUnknown));
        assert(VioGpuNativeScanoutModeValid(&q.m_NativeScanoutCandidate)==(result!=VioGpuHostContextUnknown));
        drained(q);
    }
    for(bool revoke : {false,true}) {
        VioGpuAdapter q; VIOGPU_NATIVE_SCANOUT_MODE candidate{};
        assert(q.ReserveNativeScanoutMode(&candidate));
        q.m_CtrlQueue.failAfterCommit=!revoke; q.m_CtrlQueue.mutateAfterCommit=revoke;
        assert(q.CommitNativeScanoutMode(&candidate)==VioGpuHostContextConfirmed);
        assert(q.m_CtrlQueue.afterCommitQueries==0 && VioGpuNativeScanoutModeEqual(&candidate,&q.m_NativeScanoutCommitted));
        drained(q); // Revocation cannot turn a completed mode commit into a rollback.
    }

    VioGpuAdapter headless;
    headless.m_CtrlQueue.profile={1,64,0,0,0,0,0,0,0,0,{0,0}};
    headless.ObserveNativeScanoutProfile();
    assert(headless.m_NativeScanoutObservation==1 && headless.m_NativeScanoutReconcilePending);
    assert(!headless.ReserveNativeScanoutMode(&again) && !again.LocalResetGeneration);
    headless.ObserveNativeScanoutProfile(); assert(headless.m_NativeScanoutObservation==1);
    headless.m_CtrlQueue.profile=a.m_CtrlQueue.profile;
    headless.ObserveNativeScanoutProfile(); assert(headless.m_NativeScanoutObservation==2);
    assert(headless.ReserveNativeScanoutMode(&first));
    ++headless.m_CtrlQueue.profile.ProfileGeneration;
    headless.ObserveNativeScanoutProfile();
    assert(!VioGpuNativeScanoutModeValid(&headless.m_NativeScanoutCandidate));
    assert(headless.CommitNativeScanoutMode(&first)==VioGpuHostContextNotSubmitted);
    assert(headless.ReserveNativeScanoutMode(&again) && again.Geometry.ModeGeneration>first.Geometry.ModeGeneration);
    ++headless.m_NativeContextResetGeneration;
    assert(headless.ReserveNativeScanoutMode(&first) && first.Geometry.ModeGeneration>again.Geometry.ModeGeneration);
    headless.m_CtrlQueue.failQuery=true;
    headless.ObserveNativeScanoutProfile();
    assert(!headless.m_NativeScanoutObservationValid && !VioGpuNativeScanoutModeValid(&headless.m_NativeScanoutCandidate));
    assert(!headless.m_CtrlQueue.configures); drained(headless);

    for(unsigned fault=0;fault<4;++fault) {
        VioGpuAdapter q;
        if(fault==0)q.m_u64GuestFeatures=0;
        if(fault==1)q.acquire=false;
        if(fault==2)q.m_NativeScanoutModeMutex.fail=true;
        if(fault==3)q.m_CtrlQueue.geometry.ModeGeneration=~0ULL;
        assert(!q.ReserveNativeScanoutMode(&again) && !again.LocalResetGeneration);
        assert(!q.m_CtrlQueue.configures); drained(q);
    }
    puts("PASS actual mode reservation/commit: pre-Commit ownership, Configure last, reset ambiguity, headless and revocation");
}
