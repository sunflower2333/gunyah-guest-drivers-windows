#include "viogpu_native_diagnostic_mode.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
using UINT=unsigned; using USHORT=unsigned short; using BOOLEAN=bool; using KIRQL=unsigned;
using NTSTATUS=int; using LONGLONG=long long; using VOID=void; using HANDLE=void*;
#define PAGED_CODE() ((void)0)
#define NT_SUCCESS(s) ((s)>=0)
#define DXGKDDI_INTERFACE_VERSION 0x5023
#define DXGKDDI_INTERFACE_VERSION_WDDM2_3 0x8001
constexpr bool TRUE=true,FALSE=false;
constexpr unsigned MAXUSHORT=65535,D3DDDIFMT_A8R8G8B8=21;
constexpr int STATUS_SUCCESS=0,STATUS_DEVICE_NOT_READY=-1,STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED=-2;
constexpr int STATUS_NOT_FOUND=-3,STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET=-4;
constexpr unsigned D3DKMDT_RMT_GRAPHICS=1,D3DKMDT_CB_SCRGB=1,D3DKMDT_CB_SRGB=2,D3DKMDT_PVAM_DIRECT=1,
    D3DKMDT_VPPR_ROTATE90=2,D3DKMDT_VPPS_IDENTITY=1,D3DKMDT_MP_NOTPREFERRED=0,D3DKMDT_MCO_DRIVER=1;
void KeAcquireSpinLock(unsigned*,unsigned *irql) { *irql=0; }
void KeReleaseSpinLock(unsigned*,unsigned) {}
struct Region { unsigned cx,cy; };
struct Rational { unsigned Numerator,Denominator; };
struct D3DKMDT_VIDEO_SIGNAL_INFO {
    Region ActiveSize{},TotalSize{}; unsigned long long PixelRate{};
    unsigned ScanLineOrdering{}; Rational HSyncFreq{},VSyncFreq{};
};
struct D3DKMDT_VIDPN_SOURCE_MODE {
    struct { struct { Region PrimSurfSize,VisibleRegionSize; unsigned Stride,PixelFormat;
                     unsigned ColorBasis{},PixelValueAccessMode{}; } Graphics; } Format;
    unsigned Type{};
};
struct D3DKMDT_VIDPN_TARGET_MODE { D3DKMDT_VIDEO_SIGNAL_INFO VideoSignalInfo; unsigned Preference; };
struct D3DKMDT_MONITOR_SOURCE_MODE { D3DKMDT_VIDEO_SIGNAL_INFO VideoSignalInfo; unsigned Preference,Origin,ColorBasis;
    struct { unsigned FirstChannel,SecondChannel,ThirdChannel,FourthChannel; } ColorCoeffDynamicRanges; };
template<typename T> struct ModeSet {
    T entry{}; unsigned creates{},adds{},releases{}; int createStatus{},addStatus{},releaseStatus{};
    static int create(ModeSet *set,T **out) { ++set->creates; *out=&set->entry; return set->createStatus; }
    static int add(ModeSet *set,T *entry) { assert(entry==&set->entry); ++set->adds; return set->addStatus; }
    static int release(ModeSet *set,const T *entry) { assert(entry==&set->entry); ++set->releases; return set->releaseStatus; }
};
template<typename T> struct ModeInterface {
    decltype(&ModeSet<T>::create) pfnCreateNewModeInfo=ModeSet<T>::create;
    decltype(&ModeSet<T>::add) pfnAddMode=ModeSet<T>::add;
    decltype(&ModeSet<T>::release) pfnReleaseModeInfo=ModeSet<T>::release;
};
using DXGK_VIDPNSOURCEMODESET_INTERFACE=ModeInterface<D3DKMDT_VIDPN_SOURCE_MODE>;
using DXGK_VIDPNTARGETMODESET_INTERFACE=ModeInterface<D3DKMDT_VIDPN_TARGET_MODE>;
using D3DKMDT_HVIDPNSOURCEMODESET=ModeSet<D3DKMDT_VIDPN_SOURCE_MODE>*;
using D3DKMDT_HVIDPNTARGETMODESET=ModeSet<D3DKMDT_VIDPN_TARGET_MODE>*;
struct DXGKARG_RECOMMENDMONITORMODES {
    ModeInterface<D3DKMDT_MONITOR_SOURCE_MODE> *pMonitorSourceModeSetInterface;
    ModeSet<D3DKMDT_MONITOR_SOURCE_MODE> *hMonitorSourceModeSet;
};
struct D3DKMDT_VIDPN_PRESENT_PATH {
    unsigned VidPnSourceId,VidPnTargetId;
    struct { unsigned Rotation,Scaling; } ContentTransformation;
};
struct VIDEO_MODE_INFORMATION { unsigned VisScreenWidth=3040,VisScreenHeight=1904,ScreenStride=12160; };
struct CURRENT_MODE {
    struct { unsigned Width=3040,Height=1904,Pitch=12160,ColorFormat=22; } DispInfo;
    unsigned SrcModeWidth=3040,SrcModeHeight=1904,Rotation=1,Scaling=1;
    struct { bool FullscreenPresent{},FrameBufferIsActive=true; } Flags;
    void *FrameBuffer=reinterpret_cast<void*>(0x1234);
};
enum VIOGPU_HOST_CONTEXT_RESULT { VioGpuHostContextNotSubmitted,VioGpuHostContextConfirmed,
    VioGpuHostContextRejected,VioGpuHostContextUnknown };
struct Backend {
    VIDEO_MODE_INFORMATION info;
    VIOGPU_DISPLAY_TIMING timing=VioGpuVirtualTiming(3040,1904,165);
    bool eligible{},revoked{},committed{}; unsigned commits{},selects{};
    VIOGPU_HOST_CONTEXT_RESULT result=VioGpuHostContextConfirmed;
    unsigned short GetCurrentModeIndex() { return 0; }
    unsigned GetModeCount() { return 1; }
    VIDEO_MODE_INFORMATION *GetModeInfo(unsigned index) { assert(!index); return &info; }
    VIOGPU_DISPLAY_TIMING GetModeTiming(unsigned index) { assert(!index); return timing; }
    bool NativeScanoutModeEligible(const VIOGPU_NATIVE_SCANOUT_MODE*,bool only) { assert(only); return eligible&&!revoked; }
    VIOGPU_HOST_CONTEXT_RESULT CommitNativeScanoutMode(const VIOGPU_NATIVE_SCANOUT_MODE *mode) {
        assert(VioGpuNativeScanoutModeValid(mode)); ++commits;
        if(revoked)return VioGpuHostContextRejected;
        if(result==VioGpuHostContextConfirmed)eligible=committed=true;
        return result;
    }
    void SetCurrentModeIndex(unsigned index) { assert(!index); ++selects; }
};
struct VioGpuDod {
    Backend hw; Backend *m_pHWDevice=&hw; CURRENT_MODE m_CurrentMode;
    VIOGPU_NATIVE_SCANOUT_MODE reserved{112,{1,64,1904,3040,3040,1904,3,0,83,1,{0,0}},
                                            {1,64,7,3,1904,3040,3040,1904,23,29,{0,0}}};
    VIOGPU_DISPLAY_TIMING m_CrtcTiming=VioGpuVirtualTiming(3040,1904,165);
    unsigned m_CrtcTimingLock{}; LONGLONG m_CrtcPeriodTicks=7,m_CrtcEpoch=8;
    bool enabled=true,available=true,rundown=true,held{},flipHeld{},primaryValid=true,failTiming{};
    unsigned reserveCalls{},timings{},drains{},clears{},disarms{},resets{};
    bool NativeScanoutDiagnosticEnabled() { return enabled; }
    bool ReserveNativeScanoutMode(VIOGPU_NATIVE_SCANOUT_MODE *out) {
        ++reserveCalls; assert(!hw.committed); *out=reserved; return enabled&&available;
    }
    bool AcquireNativeSubmissionOperation() { assert(!held); return held=rundown; }
    void ReleaseNativeSubmissionOperation() { assert(held&&!flipHeld); held=false; }
    void AcquireFlipApply() { assert(held&&!flipHeld); flipHeld=true; }
    void ReleaseFlipApply() { assert(flipHeld); flipHeld=false; }
    void BuildVideoSignalInfo(D3DKMDT_VIDEO_SIGNAL_INFO *out,VIDEO_MODE_INFORMATION *info) {
        assert(info==&hw.info); const auto t=hw.timing;
        out->ActiveSize={t.Width,t.Height}; out->TotalSize={t.TotalWidth,t.TotalHeight};
        out->PixelRate=t.PixelClock; out->ScanLineOrdering=1;
        assert(VioGpuTimingRational(t.PixelClock,t.TotalWidth,out->HSyncFreq.Numerator,out->HSyncFreq.Denominator));
        assert(VioGpuTimingRational(t.PixelClock,static_cast<unsigned long long>(t.TotalWidth)*t.TotalHeight,
                                   out->VSyncFreq.Numerator,out->VSyncFreq.Denominator));
    }
    int SetCrtcTiming(VIOGPU_DISPLAY_TIMING timing) {
        assert(held&&flipHeld&&!hw.committed); ++timings;
        if(failTiming)return -1;
        m_CrtcTiming=timing; return 0;
    }
    void DisarmCrtcVsyncTimer() { ++disarms; }
    unsigned TakePendingFlip() { assert(flipHeld&&hw.committed); ++drains; return 0; }
    void SetCrtcVsyncPrimaryAddress(unsigned address) { assert(!address&&hw.committed); ++clears; }
    void RequestHardwareResetAtAnyIrql() { ++resets; }
    void RecordNativeDiagnosticDdi(const D3DKMDT_VIDPN_SOURCE_MODE*,const D3DKMDT_VIDPN_PRESENT_PATH*,
        const D3DKMDT_VIDEO_SIGNAL_INFO*,HANDLE,unsigned stage,int,const VIOGPU_NATIVE_SCANOUT_MODE*) {
        assert(held&&flipHeld&&stage==2);
    }
    BOOLEAN PrepareNativeDiagnosticMode(VIOGPU_NATIVE_SCANOUT_MODE*,VIDEO_MODE_INFORMATION*,
        VIOGPU_DISPLAY_TIMING*,D3DKMDT_VIDEO_SIGNAL_INFO*,USHORT*);
    NTSTATUS SetNativeDiagnosticModeAndPath(const D3DKMDT_VIDPN_SOURCE_MODE*,
        const D3DKMDT_VIDPN_PRESENT_PATH*,const D3DKMDT_VIDEO_SIGNAL_INFO*,HANDLE);
    NTSTATUS AddNativeDiagnosticSourceMode(const DXGK_VIDPNSOURCEMODESET_INTERFACE*,
        D3DKMDT_HVIDPNSOURCEMODESET,const D3DKMDT_VIDPN_TARGET_MODE*);
    NTSTATUS AddNativeDiagnosticTargetMode(const DXGK_VIDPNTARGETMODESET_INTERFACE*,
        D3DKMDT_HVIDPNTARGETMODESET,const D3DKMDT_VIDPN_SOURCE_MODE*);
    NTSTATUS AddNativeDiagnosticMonitorMode(const DXGKARG_RECOMMENDMONITORMODES*);
};
bool VioGpuWddmValidateDiagnosticPrimary(VioGpuDod *dod,HANDLE,const VIOGPU_NATIVE_SCANOUT_MODE*) {
    assert(dod->held&&dod->flipHeld&&!dod->hw.committed); return dod->primaryValid;
}
// INSERT_PRODUCTION
static D3DKMDT_VIDEO_SIGNAL_INFO signal(VioGpuDod &dod) {
    VIOGPU_NATIVE_SCANOUT_MODE mode; VIDEO_MODE_INFORMATION info; VIOGPU_DISPLAY_TIMING timing;
    D3DKMDT_VIDEO_SIGNAL_INFO result; USHORT index;
    assert(dod.PrepareNativeDiagnosticMode(&mode,&info,&timing,&result,&index));
    assert(result.ActiveSize.cx==1904 && result.ActiveSize.cy==3040);
    assert(result.TotalSize.cx==1939 && result.TotalSize.cy==3200);
    assert(result.VSyncFreq.Numerator==165 && result.VSyncFreq.Denominator==1);
    assert(result.PixelRate==dod.hw.timing.PixelClock && result.HSyncFreq.Numerator==528000);
    return result;
}
int main() {
    const D3DKMDT_VIDPN_SOURCE_MODE source={{{{1904,3040},{1904,3040},7616,21}}};
    const D3DKMDT_VIDPN_PRESENT_PATH path={0,0,{2,1}};
    for(unsigned failure=0;failure<6;++failure) {
        VioGpuDod dod;
        ModeSet<D3DKMDT_VIDPN_SOURCE_MODE> sources;
        ModeSet<D3DKMDT_VIDPN_TARGET_MODE> targets;
        ModeSet<D3DKMDT_MONITOR_SOURCE_MODE> monitors;
        DXGK_VIDPNSOURCEMODESET_INTERFACE si;
        DXGK_VIDPNTARGETMODESET_INTERFACE ti;
        ModeInterface<D3DKMDT_MONITOR_SOURCE_MODE> mi;
        DXGKARG_RECOMMENDMONITORMODES request{&mi,&monitors};
        if(failure==1)dod.enabled=false;
        if(failure==2)sources.createStatus=targets.createStatus=monitors.createStatus=-10;
        if(failure==3)sources.addStatus=targets.addStatus=monitors.addStatus=-11;
        if(failure==4)sources.addStatus=targets.addStatus=monitors.addStatus=STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET;
        if(failure==5) {
            sources.addStatus=targets.addStatus=monitors.addStatus=-11;
            sources.releaseStatus=targets.releaseStatus=monitors.releaseStatus=-12;
        }
        const int expected=failure==2?-10:failure==3?-11:failure==5?-12:0;
        assert(dod.AddNativeDiagnosticSourceMode(&si,&sources,nullptr)==expected);
        assert(dod.AddNativeDiagnosticTargetMode(&ti,&targets,nullptr)==(failure==1?STATUS_NOT_FOUND:expected));
        assert(dod.AddNativeDiagnosticMonitorMode(&request)==expected);
        if(failure==1)assert(!sources.creates&&!targets.creates&&!monitors.creates);
        else {
            assert(sources.creates==1&&targets.creates==1&&monitors.creates==1);
            if(failure!=2) {
                assert(sources.entry.Format.Graphics.PrimSurfSize.cx==1904);
                assert(sources.entry.Format.Graphics.VisibleRegionSize.cy==3040 && sources.entry.Format.Graphics.Stride==7616);
                assert(targets.entry.VideoSignalInfo.ActiveSize.cx==1904 && targets.entry.VideoSignalInfo.TotalSize.cy==3200);
                assert(monitors.entry.VideoSignalInfo.PixelRate==dod.hw.timing.PixelClock);
            }
            assert(sources.releases==(failure>=3) && targets.releases==(failure>=3) && monitors.releases==(failure>=3));
        }
    }
    {
        VioGpuDod dod; DXGK_VIDPNSOURCEMODESET_INTERFACE si; DXGK_VIDPNTARGETMODESET_INTERFACE ti;
        ModeSet<D3DKMDT_VIDPN_SOURCE_MODE> sources; ModeSet<D3DKMDT_VIDPN_TARGET_MODE> targets;
        D3DKMDT_VIDPN_TARGET_MODE wrong{}; wrong.VideoSignalInfo.ActiveSize={3040,1904};
        assert(!dod.AddNativeDiagnosticSourceMode(&si,&sources,&wrong) && !sources.creates);
        auto wrongSource=source; wrongSource.Format.Graphics.VisibleRegionSize={3040,1904};
        assert(dod.AddNativeDiagnosticTargetMode(&ti,&targets,&wrongSource)==STATUS_NOT_FOUND && !targets.creates);
        assert(!dod.AddNativeDiagnosticTargetMode(&ti,&targets,&source) && targets.creates==1);
    }
    {
        VioGpuDod dod;
        dod.hw.timing={3040,1904,3600,1954,1160680000};
        VIOGPU_NATIVE_SCANOUT_MODE mode; VIDEO_MODE_INFORMATION info; VIOGPU_DISPLAY_TIMING timing;
        D3DKMDT_VIDEO_SIGNAL_INFO observed; USHORT index;
        assert(dod.PrepareNativeDiagnosticMode(&mode,&info,&timing,&observed,&index));
        assert(observed.TotalSize.cx==1954 && observed.TotalSize.cy==3600 && observed.PixelRate==1160680000);
        assert(observed.VSyncFreq.Numerator==1450850 && observed.VSyncFreq.Denominator==8793);
    }
    for(unsigned failure=0;failure<18;++failure) {
        VioGpuDod dod; auto target=signal(dod); auto src=source; auto p=path;
        const auto prior=dod.m_CurrentMode; const auto priorTiming=dod.m_CrtcTiming;
        if(failure==1)dod.enabled=false;
        if(failure==2)dod.available=false;
        if(failure==3)dod.rundown=false;
        if(failure==4)src.Format.Graphics.PrimSurfSize={3040,1904};
        if(failure==5)src.Format.Graphics.VisibleRegionSize={3040,1904};
        if(failure==6)src.Format.Graphics.Stride+=4;
        if(failure==7)src.Format.Graphics.PixelFormat=22;
        if(failure==8)p.ContentTransformation.Rotation=4;
        if(failure==9)p.ContentTransformation.Scaling=2;
        if(failure==10)target.TotalSize.cx+=1;
        if(failure==11)target.VSyncFreq.Numerator=60;
        if(failure==12)target.HSyncFreq.Numerator+=1;
        if(failure==13)dod.primaryValid=false;
        if(failure==14)dod.failTiming=true;
        if(failure==15)dod.hw.result=VioGpuHostContextRejected;
        if(failure==16)dod.hw.result=VioGpuHostContextUnknown;
        if(failure==17)dod.hw.revoked=true;
        const auto status=dod.SetNativeDiagnosticModeAndPath(&src,&p,&target,reinterpret_cast<void*>(7));
        assert(!dod.held&&!dod.flipHeld);
        if(failure==0) {
            assert(status==0 && dod.hw.commits==1 && dod.drains==1 && dod.clears==1);
            assert(dod.m_CurrentMode.DispInfo.Width==1904 && dod.m_CurrentMode.DispInfo.Height==3040);
            assert(!dod.m_CurrentMode.FrameBuffer && !dod.m_CurrentMode.Flags.FrameBufferIsActive);
            assert(dod.m_CurrentMode.SrcModeWidth==1904 && dod.m_CurrentMode.Rotation==2);
        } else {
            assert(status!=0 && !dod.drains && !dod.clears && !dod.hw.selects);
            assert(std::memcmp(&prior,&dod.m_CurrentMode,sizeof(prior))==0);
            assert(VioGpuSameTiming(priorTiming,dod.m_CrtcTiming));
        }
    }
    for(unsigned state=0;state<4;++state) {
        VioGpuDod dod;
        if(state!=0) {
            dod.m_CurrentMode.DispInfo={1904,3040,7616,21};
            dod.m_CurrentMode.SrcModeWidth=1904; dod.m_CurrentMode.SrcModeHeight=3040;
            dod.m_CrtcTiming={1904,3040,1939,3200,dod.hw.timing.PixelClock};
        }
        dod.hw.eligible=state>=2; dod.hw.revoked=state==3;
        const auto prior=dod.m_CurrentMode;
        const auto status=dod.SetNativeDiagnosticModeAndPath(nullptr,&path,nullptr,nullptr);
        assert((status==0)==(state==2) && !dod.hw.commits && !dod.timings);
        if(status)assert(std::memcmp(&prior,&dod.m_CurrentMode,sizeof(prior))==0);
    }
    puts("PASS actual diagnostic physical timing/Commit/Update, primary guard, Configure-last and refusal rollback");
}
