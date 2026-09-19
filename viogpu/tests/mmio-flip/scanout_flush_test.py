#!/usr/bin/env python3
"""Execute the production scanout binder with injected host publication failures."""
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
source = (ROOT / "viogpuwddm/wddmddi.cpp").read_text()
start = source.index("static NTSTATUS BindStandardPrimaryScanout(")
end = source.index("\n_Use_decl_annotations_ NTSTATUS APIENTRY", start)
body = source[start:end]

PREFIX = r'''
#include <cstdint>
#include <cstdio>
#include <cassert>
#define _In_
#define VIOGPU_NATIVE_CONTEXT 1
#define DXGKDDI_INTERFACE_VERSION 0
#define DXGKDDI_INTERFACE_VERSION_WDDM2_3 23
using UINT = unsigned; using LONG = int; using NTSTATUS = int;
using SIZE_T = size_t; using BYTE = unsigned char; using BOOLEAN = bool;
using ULONGLONG = uint64_t; using LONGLONG = int64_t;
#define TRUE true
#define FALSE false
constexpr int STATUS_SUCCESS=0, STATUS_INVALID_PARAMETER=-1, STATUS_DEVICE_NOT_READY=-2;
constexpr unsigned VIOGPU_NATIVE_RESOURCE_ID_START=0x10000;
constexpr unsigned VIOGPU_WDDM_ALLOCATION_SIGNATURE=123;
constexpr int VioGpu2DResourceGuestBlobBackingAttached=2;
enum VIOGPU_HOST_CONTEXT_RESULT { VioGpuHostContextConfirmed,
    VioGpuHostContextNotSubmitted, VioGpuHostContextRejected, VioGpuHostContextUnknown };
struct VIOGPU_PRIMARY_SCANOUT_LAYOUT { UINT Width,Height,Format,Pitch; SIZE_T Size; };
static int shareHeld=0;
struct VioGpuDod {
    bool native=false, hasFrame=false;
    int binds=0, flushes=0, latches=0, addresses=0;
    VIOGPU_HOST_CONTEXT_RESULT bind=VioGpuHostContextConfirmed, flush=VioGpuHostContextConfirmed;
    bool HasPublishedFrame() { return hasFrame; }
    bool IsZeroCopyScanoutEnabled() { return native; }
    void CountDisplayEvent(int) {}
    void RecordDisplayValue(int,LONG) {}
    void SetCrtcVsyncPrimaryAddress(ULONGLONG) { ++addresses; }
    void LatchFlippedScanout(UINT,UINT,UINT) { if(native)assert(shareHeld==1); ++latches; }
    VIOGPU_HOST_CONTEXT_RESULT Set2DScanout(UINT,UINT,UINT,UINT,UINT*,
                                           const VIOGPU_PRIMARY_SCANOUT_LAYOUT*,BOOLEAN=false) {
        if(native) { assert(shareHeld==1); }
        ++binds; return bind;
    }
    VIOGPU_HOST_CONTEXT_RESULT FlushNativeScanout(UINT,UINT,UINT) { assert(shareHeld==1); ++flushes; return flush; }
    VIOGPU_HOST_CONTEXT_RESULT Flush2DResource(UINT,UINT,UINT,int*,ULONGLONG*) {
        ++flushes; return flush;
    }
};
struct VIOGPU_WDDM_ALLOCATION {
    UINT Signature=VIOGPU_WDDM_ALLOCATION_SIGNATURE;
    VioGpuDod *Adapter=nullptr;
    UINT ResourceId=1, BlobId=0, Width=32, Height=32, Pitch=128, ShareStride=128;
    int Format=0, Resource2DState=VioGpu2DResourceGuestBlobBackingAttached, LifecycleMutex=0;
    bool PlacementValid=true;
    ULONGLONG PlacementOffset=4096, Resource2DResetGeneration=1, ShareKey=7;
    SIZE_T BackingSize=4096;
    void *ApertureAddress=nullptr;
};
static int releases;
NTSTATUS AcquireAllocationLifecycle(VIOGPU_WDDM_ALLOCATION*) { return STATUS_SUCCESS; }
bool IsStandardPrimaryAllocation(VIOGPU_WDDM_ALLOCATION*) { return true; }
bool EnsureStandard2DAllocationBacking(VIOGPU_WDDM_ALLOCATION*) { return true; }
bool VioGpuResourceBackingAttached(int) { return true; }
bool ResolveStandard2DFormat(int,UINT *out) { *out=1; return true; }
bool AcquireNativeScanoutShare(VioGpuDod*,ULONGLONG,UINT *id,ULONGLONG *size) {
    assert(!shareHeld); ++shareHeld;
    *id=VIOGPU_NATIVE_RESOURCE_ID_START; *size=4096; return true;
}
void ReleaseNativeScanoutShare(VioGpuDod*) { assert(shareHeld==1); --shareHeld; }
void KeReleaseMutex(int*,bool) { assert(!shareHeld); ++releases; }
'''
SUFFIX = r'''
int main() {
    int failures=0, cases=0;
    for (bool native : {false,true}) for (bool mode : {false,true}) {
        for (int outcome=0; outcome<4; ++outcome) {
            VioGpuDod adapter; adapter.native=native;
            adapter.flush=static_cast<VIOGPU_HOST_CONTEXT_RESULT>(outcome);
            VIOGPU_WDDM_ALLOCATION allocation; allocation.Adapter=&adapter;
            releases=0;
            const auto status=BindStandardPrimaryScanout(&adapter,&allocation,4096,mode);
            const bool ok=outcome==VioGpuHostContextConfirmed;
            if (status!=(ok?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY) ||
                adapter.binds!=1 || adapter.flushes!=1 || releases!=1 ||
                adapter.addresses!=(ok&&mode) || adapter.latches!=(ok&&!mode)) {
                printf("FAIL native=%d mode=%d flush=%d status=%d addresses=%d latches=%d\n",
                       native,mode,outcome,status,adapter.addresses,adapter.latches);
                ++failures;
            }
            ++cases;
        }
        for (int outcome=1; outcome<4; ++outcome) {
            VioGpuDod adapter; adapter.native=native;
            adapter.bind=static_cast<VIOGPU_HOST_CONTEXT_RESULT>(outcome);
            VIOGPU_WDDM_ALLOCATION allocation; allocation.Adapter=&adapter;
            releases=0;
            if (BindStandardPrimaryScanout(&adapter,&allocation,4096,mode)!=STATUS_DEVICE_NOT_READY ||
                adapter.flushes || adapter.addresses || adapter.latches || releases!=1) ++failures;
            ++cases;
        }
    }
    // Retaining an already published frame must not introduce a new host call.
    VioGpuDod adapter; adapter.hasFrame=true;
    VIOGPU_WDDM_ALLOCATION allocation; allocation.Adapter=&adapter;
    BYTE black[4096]={}; allocation.ApertureAddress=black; allocation.Resource2DState=1;
    releases=0;
    if (BindStandardPrimaryScanout(&adapter,&allocation,4096,true)!=STATUS_SUCCESS ||
        adapter.binds || adapter.flushes || adapter.addresses!=1 || releases!=1) ++failures;
    ++cases;
    // The same black CPU shadow must not suppress a live native texture.
    adapter.native=true;adapter.addresses=0;releases=0;
    if (BindStandardPrimaryScanout(&adapter,&allocation,4096,true)!=STATUS_SUCCESS ||
        adapter.binds!=1 || adapter.flushes!=1 || adapter.addresses!=1 || releases!=1 || shareHeld) ++failures;
    printf("cases=%d failures=%d\n",++cases,failures);
    return failures?1:0;
}
'''

def run(code, name, directory):
    cpp = directory / (name + ".cpp")
    exe = directory / name
    cpp.write_text("#include <initializer_list>\n" + PREFIX + code + SUFFIX)
    subprocess.run([shutil.which("c++") or "c++", "-std=c++17", "-Wall", "-Wextra",
                    "-Werror", str(cpp), "-o", str(exe)], check=True)
    return subprocess.run([str(exe)], check=False).returncode

with tempfile.TemporaryDirectory(prefix="viogpu-scanout-flush-") as tmp:
    directory = pathlib.Path(tmp)
    if run(body, "production", directory):
        raise SystemExit("Production scanout publication regression failed")
    # Semantic negative: restore the original lost-flush-status behavior.
    if body.count("result = flush;") != 1:
        raise SystemExit("Expected one publication result propagation point")
    if run(body.replace("result = flush;", "/* lost flush outcome */"), "negative", directory) == 0:
        raise SystemExit("Lost-flush-status negative was not detected")
    print("PASS: production publication cases and lost-status negative control")
    if run(body.replace('!nativeScanout && !guestBlob', '!guestBlob'), "native-black-shadow", directory) == 0:
        raise SystemExit("Native black CPU shadow negative was not detected")
    print("PASS: native publication is independent of the unused CPU shadow")
