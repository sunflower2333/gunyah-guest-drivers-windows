#include <cassert>
#include <cstdio>
#include <cstring>
using BOOLEAN=bool;
#define FALSE false
#define TRUE true
#define RtlZeroMemory(p,n) std::memset(p,0,n)
constexpr unsigned D3DKMDT_EPT_SCALING=1,D3DKMDT_EPT_ROTATION=2;
constexpr unsigned D3DKMDT_VPPS_UNPINNED=0,D3DKMDT_VPPR_UNPINNED=0;
struct Support { unsigned Identity,Centered,Rotate90,Rotate180,Rotate270,Offset,Reserved; };
using D3DKMDT_VIDPN_PRESENT_PATH_SCALING_SUPPORT=Support;
struct D3DKMDT_VIDPN_PRESENT_PATH {
    unsigned VidPnSourceId,VidPnTargetId;
    struct { unsigned Scaling,Rotation; Support ScalingSupport,RotationSupport; } ContentTransformation;
};
struct EnumRequest {
    unsigned EnumPivotType;
    struct { unsigned VidPnSourceId,VidPnTargetId; } EnumPivot;
};
struct Result { bool modified; D3DKMDT_VIDPN_PRESENT_PATH path; };
Result update(const EnumRequest *pEnumCofuncModality,const D3DKMDT_VIDPN_PRESENT_PATH *pVidPnPresentPath)
{
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
        std::memset(&path.ContentTransformation.ScalingSupport,0x5a,sizeof(Support));
        std::memset(&path.ContentTransformation.RotationSupport,0xa5,sizeof(Support));
        auto result=update(&request,&path);
        bool rotate=!(pivot==2 && !source && !target) && !(pinned&2);
        bool scale=!(pivot==1 && !source && !target) && !(pinned&1);
        assert(result.modified==(rotate||scale));
        const auto &r=result.path.ContentTransformation.RotationSupport;
        if(rotate) {
            assert(r.Identity==1 && !r.Rotate180 && !r.Rotate270 && !r.Offset && !r.Reserved && !r.Centered);
#if defined(VIOGPU_NATIVE_CONTEXT)
            assert(!r.Rotate90);
#else
            assert(r.Rotate90==1);
#endif
        } else assert(std::memcmp(&r,&path.ContentTransformation.RotationSupport,sizeof(r))==0);
        const auto &s=result.path.ContentTransformation.ScalingSupport;
        if(scale) assert(s.Identity==1 && s.Centered==1 && !s.Offset && !s.Reserved);
        else assert(std::memcmp(&s,&path.ContentTransformation.ScalingSupport,sizeof(s))==0);
        ++cases;
    }
    std::printf("PASS actual EnumVidPn path support block: %u pinned/pivot/path cases\n",cases);
}
