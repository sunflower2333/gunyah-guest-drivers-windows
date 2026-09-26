#include <cassert>
#include <cstdio>
#include <initializer_list>
using NTSTATUS=int;
#define PAGED_CODE() ((void)0)
constexpr int STATUS_SUCCESS=0,STATUS_NOT_SUPPORTED=-1;
constexpr unsigned VIRTIO_GPU_F_VIRGL=0,VIRTIO_GPU_F_RESOURCE_BLOB=3,VIRTIO_GPU_F_CONTEXT_INIT=4,
    VIRTIO_GPU_F_CREATE_GUEST_HANDLE=5,VIRTIO_GPU_F_NATIVE_AHB_V2=8,VIRTIO_GPU_F_NATIVE_AHB_RELEASE=9,
    VIRTIO_GPU_F_NATIVE_AHB_PAGING=10,VIRTIO_GPU_F_NATIVE_SCANOUT_GEOMETRY=11;
bool virtio_is_feature_enabled(unsigned long long flags,unsigned bit) { return (flags&(1ULL<<bit))!=0; }
struct Dod { bool enabled{}; bool NativeScanoutDiagnosticEnabled() { return enabled; } };
struct VioGpuAdapter {
    Dod dod; Dod *m_pVioGpuDod=&dod;
    unsigned long long m_u64HostFeatures{},guest{};
    unsigned failed=99;
    bool AckFeature(unsigned bit) { if(bit==failed)return false; guest|=1ULL<<bit; return true; }
    bool SupportsNativeAhbPaging() { return (guest&(7ULL<<8))==(7ULL<<8); }
    NTSTATUS NegotiateNativeContextFeatures();
};
// INSERT_PRODUCTION
int main() {
    for(unsigned enabled=0;enabled<2;++enabled) for(unsigned ahb=0;ahb<8;++ahb)
    for(unsigned geometry=0;geometry<2;++geometry) {
        VioGpuAdapter a; a.dod.enabled=enabled;
        a.m_u64HostFeatures=(static_cast<unsigned long long>(ahb)<<8)|(static_cast<unsigned long long>(geometry)<<11);
        assert(a.NegotiateNativeContextFeatures()==0);
        assert(((a.guest>>11)&1)==(enabled&&ahb==7&&geometry));
    }
    for(unsigned failed : {8U,9U,10U,11U}) {
        VioGpuAdapter a; a.dod.enabled=true; a.m_u64HostFeatures=15ULL<<8; a.failed=failed;
        assert(a.NegotiateNativeContextFeatures()==STATUS_NOT_SUPPORTED && !(a.guest&(1ULL<<11)));
    }
    puts("PASS actual negotiation: default-off, all AHB prerequisites, host opt-in and failed acknowledgement");
}
