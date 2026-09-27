#include <cassert>
#include <cstdio>
#include <cstring>
using BOOLEAN=bool;
#define FALSE false
#define TRUE true
#define RtlZeroMemory(p,n) std::memset(p,0,n)
constexpr unsigned D3DKMDT_EPT_SCALING=1,D3DKMDT_EPT_ROTATION=2;
constexpr unsigned D3DKMDT_VPPS_UNPINNED=0,D3DKMDT_VPPR_UNPINNED=0;
constexpr unsigned D3DKMDT_VPPR_ROTATE90=2;
// Model the WDK's distinct 32-bit support fields, including all four path
// offsets. A single fake Offset member previously hid the Offset0 contract.
#define DXGKDDI_INTERFACE_VERSION_WDDM1_3 0x4002
#define DXGKDDI_INTERFACE_VERSION 0x5023
struct D3DKMDT_VIDPN_PRESENT_PATH_SCALING_SUPPORT {
    unsigned Identity:1,Centered:1,Stretched:1,AspectRatioCenteredMax:1,Custom:1,Reserved:27;
};
struct RotationSupport {
    unsigned Identity:1,Rotate90:1,Rotate180:1,Rotate270:1;
    unsigned Offset0:1,Offset90:1,Offset180:1,Offset270:1,Reserved:24;
};
static_assert(sizeof(RotationSupport)==4);
static_assert(sizeof(D3DKMDT_VIDPN_PRESENT_PATH_SCALING_SUPPORT)==4);
struct D3DKMDT_VIDPN_PRESENT_PATH {
    unsigned VidPnSourceId,VidPnTargetId;
    struct {
        unsigned Scaling,Rotation;
        D3DKMDT_VIDPN_PRESENT_PATH_SCALING_SUPPORT ScalingSupport;
        struct RotationSupport RotationSupport;
    } ContentTransformation;
};
struct EnumRequest {
    unsigned EnumPivotType;
    struct { unsigned VidPnSourceId,VidPnTargetId; } EnumPivot;
};
struct Result { bool modified; D3DKMDT_VIDPN_PRESENT_PATH path; };
bool NativeScanoutDiagnosticEnabled() {
#ifdef DIAGNOSTIC_ENABLED
    return true;
#else
    return false;
#endif
}
bool SupportsNativeScanoutGeometry() { return true; }
Result update(const EnumRequest *pEnumCofuncModality,const D3DKMDT_VIDPN_PRESENT_PATH *pVidPnPresentPath)
{
    [[maybe_unused]] const bool diagnosticCofunctional=NativeScanoutDiagnosticEnabled();
// INSERT_PRODUCTION
    return {SupportFieldsModified,LocalVidPnPresentPath};
}
int main()
{
    unsigned cases=0;
    for(unsigned pivot=0;pivot<3;++pivot) for(unsigned source=0;source<2;++source)
    for(unsigned target=0;target<2;++target) for(unsigned pinned=0;pinned<4;++pinned) {
        EnumRequest request{pivot,{source,target}};
        D3DKMDT_VIDPN_PRESENT_PATH path{};
        path.ContentTransformation.Scaling=pinned&1;
        path.ContentTransformation.Rotation=(pinned>>1)&1;
        std::memset(&path.ContentTransformation.ScalingSupport,0x5a,sizeof(path.ContentTransformation.ScalingSupport));
        std::memset(&path.ContentTransformation.RotationSupport,0xa5,sizeof(path.ContentTransformation.RotationSupport));
        auto result=update(&request,&path);
        bool rotate=!(pivot==2 && !source && !target) && !(pinned&2);
        bool scale=!(pivot==1 && !source && !target) && !(pinned&1);
        assert(result.modified==(rotate||scale));
        const auto &r=result.path.ContentTransformation.RotationSupport;
        if(rotate) {
            assert(r.Identity==1 && !r.Rotate180 && !r.Rotate270 && !r.Reserved);
            // Microsoft: a primary path must expose exactly Offset0. A
            // secondary path must expose at least one offset. This adapter
            // supports no independent clone rotation, so both use Offset0.
            assert(r.Offset0==1 && !r.Offset90 && !r.Offset180 && !r.Offset270);
#if defined(VIOGPU_NATIVE_CONTEXT)
            assert(r.Rotate90==NativeScanoutDiagnosticEnabled());
#else
            assert(r.Rotate90==1);
#endif
        } else assert(std::memcmp(&r,&path.ContentTransformation.RotationSupport,sizeof(r))==0);
        const auto &s=result.path.ContentTransformation.ScalingSupport;
        if(scale) assert(s.Identity==1 && s.Centered==1 && !s.Stretched && !s.AspectRatioCenteredMax && !s.Custom && !s.Reserved);
        else assert(std::memcmp(&s,&path.ContentTransformation.ScalingSupport,sizeof(s))==0);
        ++cases;
    }
    std::printf("PASS actual EnumVidPn path support block: %u pinned/pivot/path cases\n",cases);
}
