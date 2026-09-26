#include "viogpu_scanout_geometry_wire.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
using UINT = unsigned;
using BOOLEAN = bool;
using PVOID = void *;
#define PAGED_CODE() ((void)0)
#define RtlZeroMemory(p,n) std::memset(p,0,n)
#define TRUE true
#define FALSE false
constexpr unsigned PASSIVE_LEVEL=0;
unsigned irql;
unsigned KeGetCurrentIrql() { return irql; }
constexpr unsigned VIRTIO_GPU_CMD_QUERY_SCANOUT_GEOMETRY=0xd21f;
constexpr unsigned VIRTIO_GPU_CMD_CONFIGURE_SCANOUT_GEOMETRY=0xd21b;
constexpr unsigned VIRTIO_GPU_CMD_BIND_SCANOUT_GEOMETRY=0xd21c;
constexpr unsigned VIRTIO_GPU_CMD_QUERY_SCANOUT_PROFILE=0xd221;
constexpr unsigned VIRTIO_GPU_CMD_CONFIGURE_SCANOUT_PROFILE=0xd223;
enum VIOGPU_HOST_CONTEXT_RESULT { VioGpuHostContextNotSubmitted, VioGpuHostContextConfirmed,
                                 VioGpuHostContextRejected, VioGpuHostContextUnknown };
struct GPU_VBUFFER {
    alignas(8) unsigned char bytes[112]{};
    void *response{};
    unsigned command_size{}, response_size{};
};
using PGPU_VBUFFER=GPU_VBUFFER *;
struct Pool {
    unsigned live{}; bool fail{};
    void *AllocateMemory(size_t n) { if(fail) return nullptr; ++live; return std::malloc(n); }
    void FreeMemory(void *p) { assert(live); --live; std::free(p); }
};
struct CtrlQueue {
    Pool pool; Pool *m_pBuf=&pool;
    unsigned begins{}, sends{}, releases{}, responseVariant{};
    bool beginFails{}, commandFails{}, timeout{}, locked{};
    PGPU_VBUFFER retained{};
    VIOGPU_HOST_CONTEXT_RESULT noDataResult=VioGpuHostContextConfirmed;
    VIOGPU_SCANOUT_GEOMETRY expected={1,64,1904,3040,3040,1904,1,0,7,11,{0,0}};
    VIOGPU_SCANOUT_PROFILE expectedProfile={1,64,7,1,1904,3040,3040,1904,23,29,{0,0}};
    bool IsStandard2DResourceId(unsigned id) { return id && id<0x80000000U; }
    bool BeginSynchronousRequest() { ++begins; if(beginFails)return false; assert(!locked); locked=true; return true; }
    void EndSynchronousRequest() { assert(locked); locked=false; }
    void *AllocCmd(PGPU_VBUFFER *out, unsigned size) {
        if(commandFails)return nullptr;
        assert(size==96 || size==48 || size==112);
        *out=new GPU_VBUFFER; (*out)->command_size=size; return (*out)->bytes;
    }
    void *AllocCmdResp(PGPU_VBUFFER *out, unsigned size, void *response, unsigned responseSize) {
        if(commandFails)return nullptr;
        assert(size==32 && (responseSize==96 || responseSize==112));
        *out=new GPU_VBUFFER; (*out)->command_size=size; (*out)->response=response; return (*out)->bytes;
    }
    void ReleaseBuffer(PGPU_VBUFFER p) {
        ++releases; if(p->response)pool.FreeMemory(p->response); delete p;
    }
    bool SubmitSynchronousLocked(PGPU_VBUFFER p, bool *release) {
        assert(locked); ++sends;
        const auto *q=reinterpret_cast<VIOGPU_QUERY_SCANOUT_GEOMETRY *>(p->bytes);
        assert((q->Header.Type==0xd21f || q->Header.Type==0xd221) && q->ScanoutId==0 && q->Reserved==0);
        for(unsigned i=4;i<24;++i) assert(p->bytes[i]==0);
        if(timeout) { *release=false; retained=p; return false; }
        if(q->Header.Type==0xd221) {
            auto *r=static_cast<VIOGPU_SCANOUT_PROFILE_RESPONSE *>(p->response);
            *r={}; r->Header.Type=0xd222; r->Profile=expectedProfile; r->HostResetGeneration=7;
            p->response_size=112;
            switch(responseVariant) {
            case 1: p->response_size=111; break;
            case 2: p->response_size=113; break;
            case 3: r->Header.Flags=1; break;
            case 4: r->Profile.Flags=3; break;
            case 5: r->HostResetGeneration=0; break;
            case 6: r->Profile.StorageWidth=3040; break;
            case 7: r->Profile.ReservedTail[1]=1; break;
            case 8: r->ScanoutId=1; break;
            case 9: r->Header.Type=0x1205; p->response_size=24; break;
            case 10: r->Profile={1,64,0,0,0,0,0,0,23,30,{0,0}}; break;
            case 11: r->Reserved=1; break;
            case 12: r->ReservedTail=1; break;
            }
            return true;
        }
        auto *r=static_cast<VIOGPU_SCANOUT_GEOMETRY_RESPONSE *>(p->response);
        *r={}; r->Header.Type=0xd220; r->Flags=1; r->Geometry=expected; p->response_size=96;
        switch(responseVariant) {
        case 1: p->response_size=95; break;
        case 2: p->response_size=97; break;
        case 3: r->Header.Flags=1; break;
        case 4: r->Flags=2; break;
        case 5: r->Geometry.ModeGeneration=0; break;
        case 6: r->Geometry.StorageWidth=3040; break;
        case 7: r->Geometry.ReservedTail[1]=1; break;
        case 8: r->ScanoutId=1; break;
        case 9: r->Header.Type=0x1205; p->response_size=24; break;
        case 10: r->Flags=0; r->Geometry={1,64,3040,1904,3040,1904,0,0,7,0,{0,0}}; break;
        }
        return true;
    }
    VIOGPU_HOST_CONTEXT_RESULT SubmitSynchronousNoDataLocked(PGPU_VBUFFER p) {
        assert(locked); ++sends;
        auto *h=reinterpret_cast<VIOGPU_GEOMETRY_CONTROL_HEADER *>(p->bytes);
        for(unsigned i=4;i<24;++i) assert(p->bytes[i]==0);
        if(h->Type==0xd21b) {
            assert(p->command_size==96);
            auto *c=reinterpret_cast<VIOGPU_CONFIGURE_SCANOUT_GEOMETRY *>(p->bytes);
            assert(c->ScanoutId==0 && c->Reserved==0 && VioGpuScanoutGeometryEqual(&c->Geometry,&expected));
        } else if(h->Type==0xd223) {
            assert(p->command_size==112);
            auto *c=reinterpret_cast<VIOGPU_CONFIGURE_SCANOUT_PROFILE *>(p->bytes);
            assert(c->ScanoutId==0 && c->Reserved==0 && VioGpuScanoutGeometryEqual(&c->Geometry,&expected));
            assert(c->EndpointGeneration==23 && c->ProfileGeneration==29);
        } else {
            assert(h->Type==0xd21c && p->command_size==48);
            auto *b=reinterpret_cast<VIOGPU_BIND_SCANOUT_GEOMETRY *>(p->bytes);
            assert(b->ResourceId==42 && b->ScanoutId==0 && b->HostResetGeneration==7 && b->ModeGeneration==11);
        }
        ReleaseBuffer(p); return noDataResult;
    }
    BOOLEAN QueryScanoutGeometry(VIOGPU_SCANOUT_GEOMETRY_RESPONSE *,BOOLEAN);
    BOOLEAN QueryScanoutProfile(VIOGPU_SCANOUT_PROFILE_RESPONSE *,BOOLEAN);
    VIOGPU_HOST_CONTEXT_RESULT ConfigureScanoutProfile(const VIOGPU_SCANOUT_GEOMETRY *,
                                                      const VIOGPU_SCANOUT_PROFILE_RESPONSE *,BOOLEAN);
    VIOGPU_HOST_CONTEXT_RESULT ConfigureScanoutGeometry(const VIOGPU_SCANOUT_GEOMETRY *,BOOLEAN);
    VIOGPU_HOST_CONTEXT_RESULT BindScanoutGeometry(UINT,const VIOGPU_SCANOUT_GEOMETRY *,BOOLEAN);
    ~CtrlQueue() { assert(!locked && !retained && !pool.live); }
};
// INSERT_PRODUCTION
int main() {
    CtrlQueue q; VIOGPU_SCANOUT_GEOMETRY_RESPONSE out{};
    out.Flags=123;
    assert(!q.QueryScanoutGeometry(&out,false) && out.Flags==0 && q.begins==0);
    assert(q.ConfigureScanoutGeometry(&q.expected,false)==VioGpuHostContextNotSubmitted);
    assert(q.BindScanoutGeometry(42,&q.expected,false)==VioGpuHostContextNotSubmitted);
    assert(q.begins==0 && q.sends==0);
    assert(q.QueryScanoutGeometry(&out,true) && VioGpuScanoutGeometryEqual(&out.Geometry,&q.expected));
    for(q.responseVariant=1;q.responseVariant!=10;++q.responseVariant) {
        out.Flags=123;
        assert(!q.QueryScanoutGeometry(&out,true) && out.Flags==0 && out.Geometry.HostResetGeneration==0);
    }
    assert(q.QueryScanoutGeometry(&out,true) && out.Flags==0 && out.Geometry.ModeGeneration==0);
    q.responseVariant=0;
    q.pool.fail=true; assert(!q.QueryScanoutGeometry(&out,true)); q.pool.fail=false;
    q.commandFails=true;
    assert(!q.QueryScanoutGeometry(&out,true) && !q.pool.live);
    assert(q.ConfigureScanoutGeometry(&q.expected,true)==VioGpuHostContextNotSubmitted);
    assert(q.BindScanoutGeometry(42,&q.expected,true)==VioGpuHostContextNotSubmitted);
    q.commandFails=false;
    q.beginFails=true; assert(!q.QueryScanoutGeometry(&out,true)); q.beginFails=false;
    q.timeout=true; assert(!q.QueryScanoutGeometry(&out,true) && q.retained && q.pool.live==1);
    q.ReleaseBuffer(q.retained); q.retained=nullptr; q.timeout=false; // Real timeout retirement owns storage.
    for(auto result : {VioGpuHostContextConfirmed,VioGpuHostContextRejected,VioGpuHostContextUnknown}) {
        q.noDataResult=result;
        assert(q.ConfigureScanoutGeometry(&q.expected,true)==result);
        assert(q.BindScanoutGeometry(42,&q.expected,true)==result);
    }
    auto bad=q.expected; bad.ModeGeneration=0;
    const unsigned before=q.sends;
    assert(q.ConfigureScanoutGeometry(&bad,true)==VioGpuHostContextNotSubmitted);
    assert(q.BindScanoutGeometry(42,&bad,true)==VioGpuHostContextNotSubmitted);
    assert(q.BindScanoutGeometry(0,&q.expected,true)==VioGpuHostContextNotSubmitted);
    assert(q.BindScanoutGeometry(0x80000000U,&q.expected,true)==VioGpuHostContextNotSubmitted);
    irql=2;
    assert(!q.QueryScanoutGeometry(&out,true));
    assert(q.ConfigureScanoutGeometry(&q.expected,true)==VioGpuHostContextNotSubmitted);
    assert(q.BindScanoutGeometry(42,&q.expected,true)==VioGpuHostContextNotSubmitted);
    irql=0;
    assert(q.sends==before && !q.pool.live && !q.locked);
    puts("PASS extracted QUERY/CONFIGURE/BIND exact wire, malformed replies, negotiation and transport failures");

    VIOGPU_SCANOUT_PROFILE_RESPONSE profile{};
    const unsigned profileBegins=q.begins;
    assert(!q.QueryScanoutProfile(&profile,false) && profile.HostResetGeneration==0);
    assert(q.ConfigureScanoutProfile(&q.expected,nullptr,false)==VioGpuHostContextNotSubmitted);
    assert(q.begins==profileBegins);
    assert(q.QueryScanoutProfile(&profile,true) && profile.Profile.ProfileGeneration==29);
    for(q.responseVariant=1;q.responseVariant!=13;++q.responseVariant) {
        VIOGPU_SCANOUT_PROFILE_RESPONSE outProfile{}; outProfile.HostResetGeneration=123;
        bool result=q.QueryScanoutProfile(&outProfile,true);
        if(q.responseVariant==10) {
            assert(result && outProfile.Profile.Flags==0 && outProfile.Profile.ProfileGeneration==30);
            assert(q.ConfigureScanoutProfile(&q.expected,&outProfile,true)==VioGpuHostContextNotSubmitted);
        } else assert(!result && outProfile.HostResetGeneration==0);
    }
    q.responseVariant=0;
    for(auto result : {VioGpuHostContextConfirmed,VioGpuHostContextRejected,VioGpuHostContextUnknown}) {
        q.noDataResult=result;
        assert(q.ConfigureScanoutProfile(&q.expected,&profile,true)==result);
    }
    const unsigned profileSends=q.sends;
    auto badGeometry=q.expected; badGeometry.HostResetGeneration=8;
    assert(q.ConfigureScanoutProfile(&badGeometry,&profile,true)==VioGpuHostContextNotSubmitted);
    badGeometry=q.expected; badGeometry.ContentRotationCw=3;
    assert(q.ConfigureScanoutProfile(&badGeometry,&profile,true)==VioGpuHostContextNotSubmitted);
    assert(q.ConfigureScanoutProfile(nullptr,&profile,true)==VioGpuHostContextNotSubmitted);
    assert(q.ConfigureScanoutProfile(&q.expected,nullptr,true)==VioGpuHostContextNotSubmitted);
    auto badProfile=profile; badProfile.Profile.EndpointGeneration=0;
    assert(q.ConfigureScanoutProfile(&q.expected,&badProfile,true)==VioGpuHostContextNotSubmitted);
    irql=2;
    assert(!q.QueryScanoutProfile(&profile,true));
    assert(q.ConfigureScanoutProfile(&q.expected,&badProfile,true)==VioGpuHostContextNotSubmitted);
    irql=0;
    assert(q.sends==profileSends);
    q.pool.fail=true; assert(!q.QueryScanoutProfile(&profile,true)); q.pool.fail=false;
    q.commandFails=true; assert(!q.QueryScanoutProfile(&profile,true) && !q.pool.live); q.commandFails=false;
    q.beginFails=true; assert(!q.QueryScanoutProfile(&profile,true)); q.beginFails=false;
    assert(q.QueryScanoutProfile(&profile,true));
    q.commandFails=true;
    assert(q.ConfigureScanoutProfile(&q.expected,&profile,true)==VioGpuHostContextNotSubmitted);
    q.commandFails=false;
    q.timeout=true; assert(!q.QueryScanoutProfile(&profile,true) && q.retained && q.pool.live==1);
    q.ReleaseBuffer(q.retained); q.retained=nullptr; q.timeout=false;
    assert(!q.pool.live && !q.locked);
    puts("PASS extracted profile QUERY32/CONFIGURE112, unavailable, exact host reset/tokens and timeout ownership");
}
