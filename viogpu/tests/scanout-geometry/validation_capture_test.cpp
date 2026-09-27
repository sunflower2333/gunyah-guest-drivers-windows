// Kernel services used by the extracted production capture writer.
#include <atomic>
#include <mutex>
#include <thread>
using SIZE_T=size_t;
using UNICODE_STRING=const wchar_t*;
constexpr unsigned Executive=0,KernelMode=0,PLUGPLAY_REGKEY_DRIVER=1,KEY_SET_VALUE=2,REG_BINARY=3;
std::atomic<unsigned long long> ticks{1};
thread_local bool locked=false;
bool waitFailure=false,openFailure=false;
unsigned opens=0,writes=0,closes=0;
std::vector<unsigned char> published;
#define RtlCopyMemory(p,s,n) std::memcpy(p,s,n)
unsigned long long KeQueryInterruptTime() { return ticks.fetch_add(1); }
int KeWaitForSingleObject(std::mutex *m,unsigned,unsigned,bool,void*) {
    if(waitFailure)return -99;
    assert(!locked); m->lock(); locked=true; return 0;
}
void KeReleaseMutex(std::mutex *m,bool) { assert(locked); locked=false; m->unlock(); }
int IoOpenDeviceRegistryKey(void*,unsigned,unsigned,void **out) {
    assert(locked); ++opens; if(openFailure)return -99;
    *out=reinterpret_cast<void*>(1); return 0;
}
void RtlInitUnicodeString(UNICODE_STRING *out,const wchar_t *text) { *out=text; }
int ZwSetValueKey(void*,UNICODE_STRING *name,unsigned,unsigned type,void *data,unsigned long size) {
    assert(locked && type==REG_BINARY && std::wcscmp(*name,L"NativeScanoutValidationCapture")==0);
    ++writes; const auto *p=static_cast<unsigned char*>(data); published.assign(p,p+size); return 0;
}
void ZwClose(void*) { assert(locked); ++closes; }

// INSERT_MEMBERS
    std::mutex m_NativeDiagnosticCaptureMutex;
    VIOGPU_NATIVE_VALIDATION_CAPTURE m_NativeValidationCapture{};
    void *m_pPhysicalDevice=nullptr;
    unsigned m_NativeDiagnosticCaptureCount=11,m_NativeDiagnosticCursorMask=3;
    void RecordNativeValidation(VIOGPU_NATIVE_VALIDATION_RECORD*,NTSTATUS);

// INSERT_MAIN
int main() {
    VioGpuDod dod;
    const D3DKMDT_VIDPN_SOURCE_MODE source={{{{1904,3040},{1904,3040},7616,21,2,1}},D3DKMDT_RMT_GRAPHICS};
    D3DKMDT_VIDPN_PRESENT_PATH path={0,0,{2,1}};
    path.ContentTransformation.RotationSupport.Rotate90=1;
    path.ContentTransformation.RotationSupport.Offset0=1;
    path.ContentTransformation.ScalingSupport.Identity=1;
    dod.hw.timing={3040,1904,3600,1954,1160680000ULL};
    VIOGPU_NATIVE_SCANOUT_MODE mode; VIDEO_MODE_INFORMATION info; VIOGPU_DISPLAY_TIMING timing;
    D3DKMDT_VIDEO_SIGNAL_INFO expected; USHORT index;
    assert(dod.PrepareNativeDiagnosticMode(&mode,&info,&timing,&expected,&index));
    D3DKMDT_VIDPN_TARGET_MODE target{expected,0};
    VIOGPU_NATIVE_VALIDATION_RECORD record{};
    record.Ddi=2; record.PivotType=2; record.Step=21;
    VioGpuCaptureValidationInputs(&record,&source,&path,&target.VideoSignalInfo);
    assert(record.Data.Flags==7 && record.Data.SourceWidth==1904 && record.Data.SourceHeight==3040);
    assert(record.Data.VisibleWidth==1904 && record.Data.VisibleHeight==3040 && record.Data.Stride==7616);
    assert(record.SourceType==1 && record.SourceColorBasis==2 && record.SourceAccessMode==1);
    assert(record.Data.TargetWidth==1904 && record.Data.TargetHeight==3040 && record.TargetScanLineOrdering==1);
    assert(record.Data.Rotation==2 && record.Data.Scaling==1 && record.ScalingSupport==1 && record.RotationSupport==18);
    const auto reserves=dod.reserveCalls;
    assert(dod.NativeDiagnosticModeCofunctional(&source,&target,&path,&record));
    assert(dod.reserveCalls==reserves+1 && record.Result==3);
    assert(record.ExpectedWidth==1904 && record.ExpectedHeight==3040);
    assert(record.ExpectedTotalWidth==1954 && record.ExpectedTotalHeight==3600);
    assert(record.ExpectedPixelClock==1160680000ULL && record.ExpectedHSyncNumerator==580340000 &&
        record.ExpectedHSyncDenominator==977 && record.ExpectedVSyncNumerator==1450850 &&
        record.ExpectedVSyncDenominator==8793 && record.ExpectedScanLineOrdering==1);
    assert(VioGpuNativeScanoutModeEqual(&mode,&record.Data.Mode));
    auto logical=source;
    logical.Format.Graphics.PrimSurfSize={3040,1904}; logical.Format.Graphics.VisibleRegionSize={3040,1904};
    logical.Format.Graphics.Stride=12160;
    assert(!dod.NativeDiagnosticModeCofunctional(&logical,&target,&path,&record) && record.Result==2);
    target.VideoSignalInfo.HSyncFreq.Denominator++;
    assert(!dod.NativeDiagnosticModeCofunctional(&source,&target,&path,&record) && record.Result==2);
    dod.available=false;
    assert(!dod.NativeDiagnosticModeCofunctional(&source,&target,&path,&record) && record.Result==1);
    assert(!record.ExpectedWidth && !record.ExpectedPixelClock);
    dod.available=true;
    VIOGPU_NATIVE_VALIDATION_RECORD textMode{};
    auto text=source; text.Type=2;
    VioGpuCaptureValidationInputs(&textMode,&text,nullptr,nullptr);
    assert(textMode.Data.Flags==1 && textMode.SourceType==2 && !textMode.Data.SourceWidth);
    // Publication remains independent of admission and Commit/cursor state.
    for(unsigned i=0;i<100;++i) { record.Step=i; dod.RecordNativeValidation(&record,-1610); }
    const auto &capture=dod.m_NativeValidationCapture;
    assert(capture.Count==32 && capture.Sequence==100 && published.size()==32*380);
    assert(capture.Records[0].Sequence==69 && capture.Records[0].Step==68);
    assert(capture.Records[31].Sequence==100 && capture.Records[31].Step==99);
    for(unsigned i=0;i<32;++i) assert(capture.Records[i].Data.Status==static_cast<unsigned>(-1610));
    assert(opens==100 && writes==100 && closes==100);
    const auto before=ticks.load(); dod.enabled=false;
    dod.RecordNativeValidation(&record,0);
    assert(capture.Sequence==100 && ticks==before && writes==100);
    dod.enabled=true; waitFailure=true;
    dod.RecordNativeValidation(&record,0); assert(capture.Sequence==100 && writes==100);
    waitFailure=false; openFailure=true;
    dod.RecordNativeValidation(&record,0); assert(capture.Sequence==101 && writes==100 && closes==100);
    openFailure=false;
    // Call-local records with mutex-serialized publication cannot interleave.
    std::vector<std::thread> threads;
    for(unsigned thread=0;thread<4;++thread) threads.emplace_back([&dod,thread] {
        for(unsigned i=0;i<50;++i) {
            VIOGPU_NATIVE_VALIDATION_RECORD local{}; local.Ddi=thread+1;
            dod.RecordNativeValidation(&local,-static_cast<int>(thread+1));
        }
    });
    for(auto &thread:threads)thread.join();
    assert(capture.Sequence==301 && capture.Count==32 && published.size()==12160);
    for(unsigned i=0;i<32;++i) {
        const auto &r=capture.Records[i];
        assert(r.Sequence==270+i && r.Data.Version==1 && r.Time100ns);
        assert(r.Data.Status==static_cast<unsigned>(-static_cast<int>(r.Ddi)));
    }
    assert(std::memcmp(published.data(),capture.Records,published.size())==0);
    assert(dod.m_NativeDiagnosticCaptureCount==11 && dod.m_NativeDiagnosticCursorMask==3);
    puts("PASS actual validation capture: raw inputs, expected timing, unchanged refusal, latest32, disabled/failure/concurrency");
}
