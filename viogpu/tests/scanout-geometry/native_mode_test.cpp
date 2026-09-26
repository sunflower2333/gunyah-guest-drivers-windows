#include "viogpu_native_mode_policy.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
using NTSTATUS=int;
using USHORT=unsigned short;
using BOOLEAN=bool;
using KIRQL=unsigned;
using LONGLONG=long long;
using VOID=void;
using HANDLE=void*;
#define CONST const
#define _In_
#define PAGED_CODE() ((void)0)
#define VIOGPU_ASSERT(c) assert(c)
#define DbgPrint(...) ((void)0)
#define NT_SUCCESS(s) ((s)>=0)
#define VIOGPU_NATIVE_CONTEXT 1
#define DXGKDDI_INTERFACE_VERSION 0x5023
#define DXGKDDI_INTERFACE_VERSION_WDDM2_3 0x8001
constexpr unsigned MAXUSHORT=65535, MAX_VIEWS=1, BITS_PER_BYTE=8;
constexpr int STATUS_SUCCESS=0, STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED=-1;
constexpr int STATUS_DEVICE_NOT_READY=-2;
constexpr unsigned D3DKMDT_VPPR_IDENTITY=1, D3DKMDT_VPPR_ROTATE90=2, D3DDDI_VSSLO_PROGRESSIVE=1;
constexpr bool TRUE=true;
constexpr bool FALSE=false;
struct VIOGPU_DISPLAY_TIMING { unsigned hz; };
bool VioGpuTimingValid(VIOGPU_DISPLAY_TIMING t) { return t.hz!=0; }
void KeAcquireSpinLock(unsigned*,unsigned *irql) { *irql=0; }
void KeReleaseSpinLock(unsigned*,unsigned) {}
struct Region { unsigned cx,cy; };
struct D3DKMDT_VIDEO_SIGNAL_INFO { Region ActiveSize,TotalSize; uint64_t PixelRate; unsigned ScanLineOrdering; };
struct D3DKMDT_VIDPN_SOURCE_MODE {
    struct { struct { Region PrimSurfSize,VisibleRegionSize; } Graphics; } Format;
};
struct D3DKMDT_VIDPN_PRESENT_PATH {
    unsigned VidPnSourceId;
    struct { unsigned Scaling,Rotation; } ContentTransformation;
};
struct DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH { D3DKMDT_VIDPN_PRESENT_PATH VidPnPresentPathInfo; };
struct CURRENT_MODE {
    struct { bool FrameBufferIsActive,FullscreenPresent; } Flags;
    struct { unsigned Width,Height,ColorFormat,Pitch; } DispInfo;
    unsigned Scaling,SrcModeWidth,SrcModeHeight,Rotation;
    void *FrameBuffer{};
};
struct VIOGPU_NATIVE_FRAMEBUFFER {};
using D3DDDIFORMAT=unsigned;
unsigned BPPFromPixelFormat(unsigned) { return 32; }
int InterlockedCompareExchange(int *p,int,int) { return *p; }
struct Backend {
    D3DKMDT_VIDEO_SIGNAL_INFO signal={{3040,1904},{3200,1930},1019040000,1};
    unsigned resizes{},selected=99;
    unsigned commits{},retires{};
    int prepareStatus{},commitStatus{};
    VIOGPU_NATIVE_FRAMEBUFFER owner;
    unsigned GetModeCount() { return 1; }
    const D3DKMDT_VIDEO_SIGNAL_INFO *GetModeInfo(unsigned) { return &signal; }
    unsigned GetModeNumber(unsigned) { return 8; }
    int SetCurrentMode(unsigned mode,CURRENT_MODE*) { assert(mode==8); ++resizes; return 0; }
    void SetCurrentModeIndex(unsigned value) { selected=value; }
    VIOGPU_DISPLAY_TIMING GetModeTiming(unsigned) { return {165}; }
    int PrepareFrameBufferMode(unsigned mode,const CURRENT_MODE *candidate,VIOGPU_NATIVE_FRAMEBUFFER **out) {
        assert(mode==8 && candidate->DispInfo.Width==3040); ++resizes;
        *out=prepareStatus==0?&owner:nullptr; return prepareStatus;
    }
    int CommitFrameBufferMode(VIOGPU_NATIVE_FRAMEBUFFER *p,CURRENT_MODE *candidate) {
        assert(p==&owner); ++commits;
        if(commitStatus==0) { candidate->FrameBuffer=&owner; candidate->Flags.FrameBufferIsActive=true; }
        return commitStatus;
    }
    void RetireFrameBufferMode(VIOGPU_NATIVE_FRAMEBUFFER *p) { assert(p==&owner); ++retires; }
};
struct VioGpuDod {
    Backend hw; Backend *m_pHWDevice=&hw;
    CURRENT_MODE m_CurrentMode={{true,false},{3040,1904,87,12160},1,3040,1904,1};
    int m_CrtcVsyncEnabled=1;
    unsigned disarms{},arms{},timings{};
    unsigned m_CrtcTimingLock{};
    VIOGPU_DISPLAY_TIMING m_CrtcTiming={60};
    LONGLONG m_CrtcPeriodTicks=1,m_CrtcEpoch=1;
    bool rundown=true,held{},flipHeld{};
    unsigned pendingDrains{},primaryClears{},resets{};
    int timingStatus{};
    void BuildVideoSignalInfo(D3DKMDT_VIDEO_SIGNAL_INFO *out,const D3DKMDT_VIDEO_SIGNAL_INFO *in) { *out=*in; }
    void DisarmCrtcVsyncTimer() { ++disarms; }
    void ArmCrtcVsyncTimer() { ++arms; }
    int SetCrtcTiming(VIOGPU_DISPLAY_TIMING timing) {
        ++timings; ++disarms;
        if(timingStatus!=0) return timingStatus;
        m_CrtcTiming=timing; return 0;
    }
    bool AcquireNativeSubmissionOperation() { assert(!held); return held=rundown; }
    void ReleaseNativeSubmissionOperation() { assert(held); held=false; }
    void AcquireFlipApply() { assert(held&&!flipHeld); flipHeld=true; }
    void ReleaseFlipApply() { assert(flipHeld); flipHeld=false; }
    unsigned TakePendingFlip() { assert(flipHeld); ++pendingDrains; return 0; }
    void SetCrtcVsyncPrimaryAddress(unsigned address) { assert(address==0); ++primaryClears; }
    void RequestHardwareResetAtAnyIrql() { ++resets; }
    int IsVidPnPathFieldsValid(const D3DKMDT_VIDPN_PRESENT_PATH *) { return 0; }
    bool NativeScanoutDiagnosticEnabled() { return false; }
    bool NativeScanoutBindingCurrent(const void*) { return true; }
    int SetNativeDiagnosticModeAndPath(const D3DKMDT_VIDPN_SOURCE_MODE*,const D3DKMDT_VIDPN_PRESENT_PATH*,
                                     const D3DKMDT_VIDEO_SIGNAL_INFO*,HANDLE) { assert(false); return -1; }
    NTSTATUS SetSourceModeAndPath(const D3DKMDT_VIDPN_SOURCE_MODE*,const D3DKMDT_VIDPN_PRESENT_PATH*,
                                 const D3DKMDT_VIDEO_SIGNAL_INFO*,HANDLE=nullptr);
    NTSTATUS UpdateActiveVidPnPresentPath(const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *const);
};
// INSERT_PRODUCTION
int main() {
    D3DKMDT_VIDPN_SOURCE_MODE source={{{{3040,1904},{3040,1904}}}};
    D3DKMDT_VIDPN_PRESENT_PATH path={0,{1,1}};
    VioGpuDod valid;
    assert(valid.SetSourceModeAndPath(&source,&path,&valid.hw.signal)==0);
    assert(valid.timings==1 && valid.disarms==1 && valid.hw.resizes==0 && valid.hw.selected==0);
    DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH update{path};
    assert(valid.UpdateActiveVidPnPresentPath(&update)==0 && valid.m_CurrentMode.Rotation==1);
    for(unsigned rotation : {0U,2U,3U,4U,5U,254U,255U}) {
        VioGpuDod refused; auto prior=refused.m_CurrentMode;
        path.ContentTransformation.Rotation=rotation;
        assert(refused.SetSourceModeAndPath(&source,&path,&refused.hw.signal)==-1);
        update.VidPnPresentPathInfo=path;
        assert(refused.UpdateActiveVidPnPresentPath(&update)==-1);
        assert(std::memcmp(&prior,&refused.m_CurrentMode,sizeof(prior))==0);
        assert(refused.disarms==0 && refused.timings==0 && refused.hw.resizes==0 && refused.hw.selected==99);
    }
    path.ContentTransformation.Rotation=1;
    for(unsigned mismatch=0;mismatch!=4;++mismatch) {
        VioGpuDod refused; auto prior=refused.m_CurrentMode; auto target=refused.hw.signal;
        if(mismatch==0) target.ActiveSize={1904,3040};
        if(mismatch==1) ++target.TotalSize.cy;
        if(mismatch==2) ++target.PixelRate;
        if(mismatch==3) target.ScanLineOrdering=2;
        assert(refused.SetSourceModeAndPath(&source,&path,&target)==-1);
        assert(std::memcmp(&prior,&refused.m_CurrentMode,sizeof(prior))==0 && refused.disarms==0);
    }
    for(unsigned failure=0;failure!=4;++failure) {
        VioGpuDod q; q.m_CurrentMode.DispInfo.Width=1920;
        auto prior=q.m_CurrentMode;
        if(failure==0) q.rundown=false;
        if(failure==1) q.hw.prepareStatus=-2;
        if(failure==2) q.timingStatus=-2;
        if(failure==3) q.hw.commitStatus=-2;
        assert(q.SetSourceModeAndPath(&source,&path,&q.hw.signal)==-2);
        assert(std::memcmp(&prior,&q.m_CurrentMode,sizeof(prior))==0);
        assert(q.hw.selected==99 && q.m_CrtcTiming.hz==60);
        assert(!q.held && !q.flipHeld && q.pendingDrains==0 && q.primaryClears==0);
        assert(q.hw.retires==(failure>=2?1U:0U));
        if(failure==3) assert(q.timings==2);
    }
    VioGpuDod resized; resized.m_CurrentMode.DispInfo.Width=1920;
    assert(resized.SetSourceModeAndPath(&source,&path,&resized.hw.signal)==0);
    assert(resized.hw.resizes==1 && resized.hw.commits==1 && resized.hw.retires==0);
    assert(resized.m_CurrentMode.FrameBuffer==&resized.hw.owner && resized.m_CurrentMode.DispInfo.Width==3040);
    assert(resized.pendingDrains==1 && resized.primaryClears==1 && !resized.held && !resized.flipHeld);
    VioGpuDod noInitialTiming; noInitialTiming.m_CurrentMode.DispInfo.Width=1920;
    noInitialTiming.m_CrtcTiming={0}; noInitialTiming.m_CrtcPeriodTicks=0; noInitialTiming.m_CrtcEpoch=0;
    noInitialTiming.hw.commitStatus=-2;
    assert(noInitialTiming.SetSourceModeAndPath(&source,&path,&noInitialTiming.hw.signal)==-2);
    assert(noInitialTiming.m_CrtcTiming.hz==0 && noInitialTiming.m_CrtcPeriodTicks==0 && noInitialTiming.m_CrtcEpoch==0);
    puts("PASS actual mode/update gate, prepare/timing/bind failures preserve mode, timer rollback and ownership");
}
