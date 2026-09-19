#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define _Out_writes_bytes_(x)
#define _In_reads_bytes_(x)
#define _In_
using VOID = void;
using PVOID = void *;
using UCHAR = unsigned char;
using PUCHAR = UCHAR *;
using SIZE_T = size_t;
using UINT = uint32_t;
using DWORD = uint32_t;
using ULONG = uint32_t;
using LONG = int32_t;
using ULONGLONG = uint64_t;
using BOOLEAN = bool;
using NTSTATUS = int;
using D3DDDIFORMAT = int;
constexpr int D3DDDIFMT_A8B8G8R8 = 1, D3DDDIFMT_A8R8G8B8 = 2, D3DDDIFMT_X8R8G8B8 = 3;
constexpr UINT VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE = 2;
constexpr int STATUS_INVALID_PARAMETER = -1, STATUS_SUCCESS = 0, NonPagedPoolNx = 0;
struct RECT { int32_t left, top, right, bottom; };
constexpr UINT VioGpuPrimaryCacheReads=104;
struct TestAdapter { void CountDisplayEvent(UINT) {} };
static TestAdapter adapter;
struct VIOGPU_WDDM_ALLOCATION {
    PVOID ApertureAddress = nullptr, PrimaryReadCache = nullptr;
    ULONGLONG PrimaryReadCacheGeneration = 0, Resource2DResetGeneration = 1;
    SIZE_T BackingSize = 0;
    UINT Pitch = 0, Width = 0, Height = 0, Flags = 0;
    D3DDDIFORMAT Format = D3DDDIFMT_A8R8G8B8;
    bool primary = false;
};
struct VIOGPU_WDDM_PRESENT_TRANSACTION {
    VIOGPU_WDDM_ALLOCATION *Source, *Destination;
    UINT RectCount;
    RECT *DestinationSubRects;
    RECT SourceRect, DestinationRect;
    TestAdapter *Adapter = &adapter;
};
static volatile LONG g_PrimaryReadCacheCount;
static bool failAllocation;
static int failures;
LONG InterlockedIncrement(volatile LONG *p) { return ++*p; }
LONG InterlockedDecrement(volatile LONG *p) { return --*p; }
void *ExAllocatePoolUninitialized(int, SIZE_T size, int) {
    if (failAllocation) return nullptr;
    auto *p = std::malloc(size);
    if (p) std::memset(p, 0xcc, size);
    return p;
}
void ExFreePoolWithTag(void *p, int) { std::free(p); }
void RtlCopyMemory(void *d, const void *s, SIZE_T n) { std::memcpy(d, s, n); }
void KeMemoryBarrier() {}
bool IsStandardPrimaryAllocation(const VIOGPU_WDDM_ALLOCATION *a) { return a->primary; }
void check(bool value, const char *name) {
    if (!value) { ++failures; std::printf("FAIL %s\n", name); }
}
// PRODUCTION_HELPERS
void present(VIOGPU_WDDM_PRESENT_TRANSACTION *transaction) {
    auto *source = transaction->Source;
    auto *destination = transaction->Destination;
    PUCHAR sourceBase = static_cast<PUCHAR>(source->ApertureAddress);
    PUCHAR destinationBase = static_cast<PUCHAR>(destination->ApertureAddress);
    ULONGLONG copiedBytes = 0;
    // PRODUCTION_COPY
    check(copiedBytes != 0, "copy executed");
}
int main() {
    std::vector<UCHAR> input(48), backing(60, 0x99), output(48);
    for (SIZE_T i = 0; i < input.size(); ++i) input[i] = static_cast<UCHAR>(i * 7 + 1);
    VIOGPU_WDDM_ALLOCATION src, dst, readback;
    src.ApertureAddress=input.data(); src.BackingSize=48; src.Pitch=16; src.Width=3; src.Height=3;
    dst.ApertureAddress=backing.data(); dst.BackingSize=60; dst.Pitch=20; dst.Width=3; dst.Height=3; dst.primary=true;
    readback=src; readback.ApertureAddress=output.data();
    RECT full{0,0,3,3}, partial{1,1,2,2};
    VIOGPU_WDDM_PRESENT_TRANSACTION tx{&src,&dst,1,&partial,full,full};
    present(&tx);
    check(dst.PrimaryReadCache == nullptr, "partial cannot establish cache");
    tx.DestinationSubRects=&full;
    src.Format=D3DDDIFMT_A8B8G8R8;
    present(&tx);
    check(PresentReadAddress(&dst) == dst.PrimaryReadCache, "full write establishes cache");
    auto *cache=static_cast<UCHAR *>(dst.PrimaryReadCache);
    for (UINT y=0; y<3; ++y) for (UINT x=0; x<3; ++x) {
        const UCHAR *p=&input[y*16+x*4];
        const UCHAR expected[]{p[2],p[1],p[0],p[3]};
        check(std::memcmp(cache+y*20+x*4,expected,4)==0, "independent RGBA oracle");
    }
    for (UINT y=0; y<3; ++y) for (UINT x=12; x<20; ++x)
        check(cache[y*20+x]==0xcc && backing[y*20+x]==0x99, "padding untouched");
    input[20]=0x21; input[21]=0x32; input[22]=0x43; input[23]=0x54;
    tx.DestinationSubRects=&partial;
    present(&tx);
    for (UINT y=0; y<3; ++y)
        check(std::memcmp(cache+y*20,backing.data()+y*20,12)==0, "partial write coherence");
    VIOGPU_WDDM_PRESENT_TRANSACTION read{&dst,&readback,1,&full,full,full};
    present(&read);
    for (UINT y=0; y<3; ++y)
        check(std::memcmp(output.data()+y*16,backing.data()+y*20,12)==0, "cached read content");
    ++dst.Resource2DResetGeneration;
    check(PresentReadAddress(&dst)==dst.ApertureAddress, "reset retires cache");
    tx.DestinationSubRects=&full; present(&tx);
    UCHAR pageData[4]{8,9,10,11};
    check(CopyAperturePlacement(&dst,24,4,pageData,true)==0, "paging write accepted");
    check(PresentReadAddress(&dst)==dst.ApertureAddress, "paging invalidation");
    present(&tx);
    check(FillAperturePlacement(&dst,4,0x12345678)==0, "paging fill accepted");
    check(PresentReadAddress(&dst)==dst.ApertureAddress, "fill invalidation");
    present(&tx);
    check(CopyAperturePlacement(&dst,0,4,pageData,false)==0, "pageout accepted");
    check(PresentReadAddress(&dst)==dst.PrimaryReadCache, "pageout does not modify source");
    ReleasePrimaryReadCache(&dst);
    check(g_PrimaryReadCacheCount==0 && dst.PrimaryReadCache==nullptr, "release accounting");
    failAllocation=true; present(&tx);
    check(dst.PrimaryReadCache==nullptr && g_PrimaryReadCacheCount==0, "allocation failure fallback");
    failAllocation=false;
    dst.Flags=VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE; present(&tx);
    check(dst.PrimaryReadCache==nullptr, "CPU writable excluded");
    dst.Flags=0;
    VIOGPU_WDDM_ALLOCATION slots[5];
    for (auto &slot: slots) { slot=dst; tx.Destination=&slot; present(&tx); }
    check(g_PrimaryReadCacheCount==4 && slots[4].PrimaryReadCache==nullptr, "bounded pool");
    for (auto &slot: slots) ReleasePrimaryReadCache(&slot);
    check(g_PrimaryReadCacheCount==0, "all reservations released");
    // Different source origin, unequal pitches, then disjoint partial writes.
    std::vector<UCHAR> shiftedInput(7*32), shiftedBacking(4*24, 0x99);
    for (SIZE_T i=0; i<shiftedInput.size(); ++i) shiftedInput[i]=static_cast<UCHAR>(i*13+3);
    src.ApertureAddress=shiftedInput.data(); src.Pitch=32; src.Width=7; src.Height=7;
    src.BackingSize=shiftedInput.size();
    dst.ApertureAddress=shiftedBacking.data(); dst.Pitch=24; dst.Width=5; dst.Height=4;
    dst.BackingSize=shiftedBacking.size();
    RECT shiftedSource{1,2,6,6}, shiftedFull{0,0,5,4};
    tx={&src,&dst,1,&shiftedFull,shiftedSource,shiftedFull};
    present(&tx);
    cache=static_cast<UCHAR *>(dst.PrimaryReadCache);
    for (UINT y=0; y<4; ++y) for (UINT x=0; x<5; ++x) {
        const UCHAR *p=&shiftedInput[(y+2)*32+(x+1)*4];
        const UCHAR expected[]{p[2],p[1],p[0],p[3]};
        check(std::memcmp(cache+y*24+x*4,expected,4)==0, "shifted source origin");
    }
    RECT split[2]{{1,0,2,1},{3,2,5,4}};
    std::memset(shiftedInput.data(),0x42,shiftedInput.size());
    tx.RectCount=2; tx.DestinationSubRects=split; present(&tx);
    for (UINT y=0; y<4; ++y)
        check(std::memcmp(cache+y*24,shiftedBacking.data()+y*24,20)==0, "disjoint partial writes");
    ReleasePrimaryReadCache(&dst);
    std::printf("primary-cache failures=%d\n", failures);
    return failures ? 1 : 0;
}
