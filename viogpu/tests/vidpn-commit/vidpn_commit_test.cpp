#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using NTSTATUS = int32_t;
using LONG = int32_t;
using SIZE_T = size_t;
using VOID = void;
using ULONG = uint32_t;
using D3DDDI_VIDEO_PRESENT_SOURCE_ID = uint32_t;
using D3DDDI_VIDEO_PRESENT_TARGET_ID = uint32_t;
using D3DKMDT_HVIDPNTOPOLOGY = uintptr_t;
using D3DKMDT_HVIDPNSOURCEMODESET = uintptr_t;
using D3DKMDT_HVIDPNTARGETMODESET = uintptr_t;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_PENDING = 0x103;
constexpr NTSTATUS STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE = -9;
constexpr NTSTATUS STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED = -10;
constexpr NTSTATUS Refusal = -77;
constexpr uint32_t D3DDDI_ID_ALL = UINT32_MAX, D3DDDI_ID_UNINITIALIZED = UINT32_MAX - 1;
constexpr unsigned MAX_VIEWS = 1, MAX_CHILDREN = 1, DXGK_VIDPN_INTERFACE_VERSION_V1 = 1;
#ifndef _In_
#define _In_
#endif
#define CONST const
#define VIOGPU_NATIVE_CONTEXT 1
#define PAGED_CODE() ((void)0)
#define VIOGPU_ASSERT(x) assert(x)
#define NT_ASSERT(x) assert(x)
#define NT_SUCCESS(x) ((x) >= 0)
#define DbgPrint(...) ((void)0)
// The default WDDM 2.0 interface: Advanced Color color-state serialization is
// compiled out, while the explicit required-output-format contract is not.
#define DXGKDDI_INTERFACE_VERSION_WDDM2_0 0x5023
#define DXGKDDI_INTERFACE_VERSION_WDDM2_3 0x8001
#define DXGKDDI_INTERFACE_VERSION DXGKDDI_INTERFACE_VERSION_WDDM2_0
enum D3DDDIFORMAT { D3DDDIFMT_UNKNOWN = 0, D3DDDIFMT_A8R8G8B8 = 21, D3DDDIFMT_A2B10G10R10 = 31 };

struct D3DKMDT_VIDEO_SIGNAL_INFO { uint64_t PixelRate = 1160680000; };
struct D3DKMDT_VIDPN_SOURCE_MODE { struct { struct { D3DDDIFORMAT PixelFormat = D3DDDIFMT_A8R8G8B8; } Graphics; } Format; };
struct D3DKMDT_VIDPN_TARGET_MODE { D3DKMDT_VIDEO_SIGNAL_INFO VideoSignalInfo; };
struct D3DKMDT_VIDPN_PRESENT_PATH {};
struct DXGKARG_COMMITVIDPN {
    uintptr_t hFunctionalVidPn = 1;
    uint32_t AffectedVidPnSourceId = 0;
    struct { bool PathPoweredOff = false; } Flags;
};
struct Peer {
    const char* fail = "";
    bool pinSource = true, pinTarget = true;
    SIZE_T paths = 1;
    unsigned calls = 0, commits = 0;
    unsigned sources = 0, sourceModes = 0, targets = 0, targetModes = 0, pathRefs = 0;
    uint64_t currentClock = 422060000;
    std::vector<uint32_t> queriedSources;
    D3DKMDT_VIDPN_SOURCE_MODE source;
    D3DKMDT_VIDPN_TARGET_MODE target;
    D3DKMDT_VIDPN_PRESENT_PATH path;
    NTSTATUS step(const char* name) { ++calls; return std::strcmp(fail,name) ? STATUS_SUCCESS : Refusal; }
    NTSTATUS sourceStep(const char* name,uint32_t id) {
        queriedSources.push_back(id);
        return id ? STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE : step(name);
    }
    bool released() const { return !(sources || sourceModes || targets || targetModes || pathRefs); }
} peer;
struct DXGK_VIDPNSOURCEMODESET_INTERFACE {
    NTSTATUS pfnAcquirePinnedModeInfo(uintptr_t,const D3DKMDT_VIDPN_SOURCE_MODE** out) const {
        if (peer.step("sourcePinned")) return Refusal;
        if (peer.pinSource) { *out=&peer.source; ++peer.sourceModes; }
        return STATUS_SUCCESS;
    }
    NTSTATUS pfnReleaseModeInfo(uintptr_t,const D3DKMDT_VIDPN_SOURCE_MODE*) const {
        assert(peer.sourceModes); --peer.sourceModes; return STATUS_SUCCESS;
    }
} sourceInterface;
struct DXGK_VIDPNTARGETMODESET_INTERFACE {
    NTSTATUS pfnAcquirePinnedModeInfo(uintptr_t,const D3DKMDT_VIDPN_TARGET_MODE** out) const {
        if (peer.step("targetPinned")) return Refusal;
        if (peer.pinTarget) { *out=&peer.target; ++peer.targetModes; }
        return STATUS_SUCCESS;
    }
    NTSTATUS pfnReleaseModeInfo(uintptr_t,const D3DKMDT_VIDPN_TARGET_MODE*) const {
        assert(peer.targetModes); --peer.targetModes; return STATUS_SUCCESS;
    }
} targetInterface;
struct DXGK_VIDPNTOPOLOGY_INTERFACE {
    NTSTATUS pfnGetNumPaths(uintptr_t,SIZE_T* out) const {
        if (peer.step("numPaths")) return Refusal;
        *out=peer.paths; return STATUS_SUCCESS;
    }
    NTSTATUS pfnGetNumPathsFromSource(uintptr_t,uint32_t source,SIZE_T* out) const {
        NTSTATUS status=peer.sourceStep("numFrom",source);
        if (status) return status;
        *out=peer.paths; return STATUS_SUCCESS;
    }
    NTSTATUS pfnEnumPathTargetsFromSource(uintptr_t,uint32_t source,SIZE_T,uint32_t* out) const {
        NTSTATUS status=peer.sourceStep("enum",source);
        if (status) return status;
        *out=0; return STATUS_SUCCESS;
    }
    NTSTATUS pfnAcquirePathInfo(uintptr_t,uint32_t source,uint32_t,const D3DKMDT_VIDPN_PRESENT_PATH** out) const {
        NTSTATUS status=peer.sourceStep("path",source);
        if (status) return status;
        *out=&peer.path; ++peer.pathRefs; return STATUS_SUCCESS;
    }
    NTSTATUS pfnReleasePathInfo(uintptr_t,const D3DKMDT_VIDPN_PRESENT_PATH*) const {
        assert(peer.pathRefs); --peer.pathRefs; return STATUS_SUCCESS;
    }
} topologyInterface;
struct DXGK_VIDPN_INTERFACE {
    NTSTATUS pfnGetTopology(uintptr_t,uintptr_t* out,const DXGK_VIDPNTOPOLOGY_INTERFACE** table) const {
        if (peer.step("topology")) return Refusal;
        *out=2; *table=&topologyInterface; return STATUS_SUCCESS;
    }
    NTSTATUS pfnAcquireSourceModeSet(uintptr_t,uint32_t source,uintptr_t* out,
                                    const DXGK_VIDPNSOURCEMODESET_INTERFACE** table) const {
        NTSTATUS status=peer.sourceStep("sourceSet",source);
        if (status) return status;
        *out=3; *table=&sourceInterface; ++peer.sources; return STATUS_SUCCESS;
    }
    NTSTATUS pfnReleaseSourceModeSet(uintptr_t,uintptr_t) const {
        assert(peer.sources); --peer.sources; return STATUS_SUCCESS;
    }
    NTSTATUS pfnAcquireTargetModeSet(uintptr_t,uint32_t,uintptr_t* out,
                                    const DXGK_VIDPNTARGETMODESET_INTERFACE** table) const {
        if (peer.step("targetSet")) return Refusal;
        *out=4; *table=&targetInterface; ++peer.targets; return STATUS_SUCCESS;
    }
    NTSTATUS pfnReleaseTargetModeSet(uintptr_t,uintptr_t) const {
        assert(peer.targets); --peer.targets; return STATUS_SUCCESS;
    }
} vidpnInterface;
struct VioGpuDod {
    LONG counters[64] = {};
    struct {
        NTSTATUS DxgkCbQueryVidPnInterface(uintptr_t,unsigned,const DXGK_VIDPN_INTERFACE** table) const {
            if (peer.step("query")) return Refusal;
            *table=&vidpnInterface; return STATUS_SUCCESS;
        }
    } m_DxgkInterface;
    void CountDisplayEvent(unsigned index) { ++counters[index]; }
    void RecordDisplayValue(unsigned index,LONG value) { counters[index]=value; }
    NTSTATUS IsVidPnSourceModeFieldsValid(const D3DKMDT_VIDPN_SOURCE_MODE*) { return peer.step("sourceValidate"); }
    NTSTATUS IsVidPnPathFieldsValid(const D3DKMDT_VIDPN_PRESENT_PATH*) { return peer.step("pathValidate"); }
    NTSTATUS SetSourceModeAndPath(const D3DKMDT_VIDPN_SOURCE_MODE*,const D3DKMDT_VIDPN_PRESENT_PATH*,
                                  const D3DKMDT_VIDEO_SIGNAL_INFO* signal) {
        if (peer.step("setSource")) return Refusal;
        ++peer.commits; peer.currentClock=signal->PixelRate; return STATUS_SUCCESS;
    }
    NTSTATUS CommitVidPn(const DXGKARG_COMMITVIDPN*, D3DDDIFORMAT requiredOutputFormat = D3DDDIFMT_UNKNOWN);
};
// INSERT_PRODUCTION

int main() {
    unsigned checks=0,failures=0;
    auto check=[&](bool valid,const char* name) { ++checks; if (!valid) { ++failures; std::printf("FAIL %s\n",name); } };
    for (uint32_t source : {0u,D3DDDI_ID_ALL}) {
        peer=Peer{}; VioGpuDod device; DXGKARG_COMMITVIDPN request; request.AffectedVidPnSourceId=source;
        NTSTATUS status=device.CommitVidPn(&request);
        check(status==STATUS_SUCCESS && peer.commits==1 && peer.currentClock==1160680000,
              "whole and single source commit forward selected high-clock target");
        check(peer.queriedSources==std::vector<uint32_t>{0,0,0,0},"ID_ALL never enters per-source callbacks");
        check(peer.released(),"successful transaction releases all borrowed VidPN references");
        check(device.counters[5]==1 && device.counters[6]==STATUS_SUCCESS,"successful commit publishes real status");
    }
    for (const char* failure : {"query","topology","numPaths","sourceSet","sourcePinned","sourceValidate",
                               "numFrom","enum","path","pathValidate","targetSet","targetPinned","setSource"}) {
        peer=Peer{}; peer.fail=failure; VioGpuDod device; DXGKARG_COMMITVIDPN request;
        request.AffectedVidPnSourceId=D3DDDI_ID_ALL;
        NTSTATUS status=device.CommitVidPn(&request);
        check(status==Refusal && device.counters[6]==Refusal,"callback failure publishes exact result");
        check(peer.released() && !peer.commits && peer.currentClock==422060000,
              "rejected transaction releases references and preserves old selection");
    }
    for (uint32_t invalid : {1u,27u,D3DDDI_ID_UNINITIALIZED}) {
        peer=Peer{}; VioGpuDod device; DXGKARG_COMMITVIDPN request; request.AffectedVidPnSourceId=invalid;
        check(device.CommitVidPn(&request)==STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE && !peer.calls &&
              device.counters[6]==STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE,"invalid source rejected before callbacks");
    }
    for (unsigned empty=0;empty<3;++empty) {
        peer=Peer{}; VioGpuDod device; DXGKARG_COMMITVIDPN request; request.AffectedVidPnSourceId=D3DDDI_ID_ALL;
        if (empty==0) request.Flags.PathPoweredOff=true;
        if (empty==1) peer.paths=0;
        if (empty==2) peer.pinSource=false;
        check(device.CommitVidPn(&request)==STATUS_SUCCESS && peer.released() && !peer.commits &&
              device.counters[6]==STATUS_SUCCESS,"powered-off/empty/unpinned transaction preserves selection");
    }
    peer=Peer{}; peer.pinTarget=false; VioGpuDod device; DXGKARG_COMMITVIDPN request;
    check(device.CommitVidPn(&request)==STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED && peer.released() &&
          !peer.commits && device.counters[6]==STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED,
          "missing target rejected with resources released");
    // SetTimingsFromVidPn names the output format its wire color space needs.
    // A pinned primary of the other depth is refused before any mode change;
    // an unpinned source cannot satisfy an explicit requirement.
    for (D3DDDIFORMAT required : {D3DDDIFMT_A2B10G10R10, D3DDDIFMT_A8R8G8B8}) {
        peer=Peer{}; peer.source.Format.Graphics.PixelFormat=D3DDDIFMT_A8R8G8B8; VioGpuDod formatDevice;
        DXGKARG_COMMITVIDPN formatRequest;
        NTSTATUS status=formatDevice.CommitVidPn(&formatRequest,required);
        if (required==D3DDDIFMT_A8R8G8B8)
            check(status==STATUS_SUCCESS && peer.commits==1 && peer.released(),"matching required format commits");
        else
            check(status==STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED && !peer.commits && peer.released() &&
                  peer.currentClock==422060000 && formatDevice.counters[6]==STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED,
                  "eight-bit primary never satisfies a ten-bit output");
    }
    {
        peer=Peer{}; peer.pinSource=false; VioGpuDod formatDevice; DXGKARG_COMMITVIDPN formatRequest;
        check(formatDevice.CommitVidPn(&formatRequest,D3DDDIFMT_A8R8G8B8)==STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED &&
              !peer.commits && peer.released(),"unpinned source cannot satisfy a required output format");
    }
    std::printf("Production VidPN commit: %u/%u checks passed\n",checks-failures,checks);
    return failures ? 1 : 0;
}
