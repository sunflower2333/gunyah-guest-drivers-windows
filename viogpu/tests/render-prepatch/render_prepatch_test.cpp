// Platform seams only: both tested routines are extracted from wddmddi.cpp.
// Wire layout itself is covered by native-context-wire/wddm-private-abi.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>
#define _In_
#define _Inout_
#define _Out_
#define _In_reads_(n)
using UINT = uint32_t;
using ULONG = uint32_t;
using DWORD = uint32_t;
using LONG = int32_t;
using ULONGLONG = uint64_t;
using BYTE = unsigned char;
using BOOLEAN = bool;
using NTSTATUS = int32_t;
constexpr bool TRUE = true, FALSE = false;
constexpr UINT MAXUINT = UINT32_MAX, MAXULONG = UINT32_MAX;
constexpr ULONGLONG MAXULONGLONG = UINT64_MAX;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_INVALID_PARAMETER = -1, STATUS_DEVICE_NOT_READY = -2;
constexpr UINT VioGpuWddmSubmissionAllocationLimit = 1024;
constexpr UINT VIOGPU_WDDM_REFERENCE_WRITE = 2, VIOGPU_WDDM_SEGMENT_ID = 1;
constexpr UINT VIOGPU_NATIVE_RESOURCE_ID_START = 0x80000000U;
constexpr UINT VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE = 11, VIOGPU_WDDM_ALLOCATION_SIGNATURE = 12;
constexpr UINT VioGpuWddmAllocationHostNone = 0, VioGpuWddmAllocationHostLive = 1;
struct Registration
{
};
struct Adapter
{
    void RecordNativeRenderFailure(DWORD, NTSTATUS, DWORD = 0, DWORD = MAXULONG, DWORD = 0, DWORD = 0)
    {
    }
};
struct VIOGPU_WDDM_DEVICE
{
    struct Adapter *Adapter;
};
struct VIOGPU_WDDM_CONTEXT
{
    Registration NativeContext;
};
struct VIOGPU_NATIVE_CONTEXT_SNAPSHOT
{
    struct Registration *Registration;
    LONG Generation;
    ULONGLONG ResetGeneration;
    UINT ContextId;
};
struct AllocationInfo
{
    ULONGLONG Size, RequestedIova, ExpectedResetGeneration;
};
struct VIOGPU_WDDM_ALLOCATION
{
    UINT Signature = VIOGPU_WDDM_ALLOCATION_SIGNATURE;
    struct Adapter *Adapter = nullptr;
    bool Destroying = false, Native = true;
    Registration *NativeContext = nullptr;
    UINT HostState = VioGpuWddmAllocationHostLive;
    bool PlacementValid = true;
    void *ApertureMdl = this, *ApertureAddress = this;
    UINT ResourceId = VIOGPU_NATIVE_RESOURCE_ID_START + 2, BlobId = ResourceId;
    UINT ContextId = 62, BoundContextId = 62;
    LONG ContextGeneration = 3, BoundGeneration = 3;
    ULONGLONG ContextResetGeneration = 4, BoundResetGeneration = 4;
    ULONGLONG PlacementOffset = 0x2000;
    AllocationInfo PrivateData = {4096, 0x4400000000ULL, 4};
    int LifecycleMutex = 0;
};
struct VIOGPU_WDDM_OPEN_ALLOCATION
{
    UINT Signature = VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE;
    VIOGPU_WDDM_DEVICE *Device;
    VIOGPU_WDDM_ALLOCATION *Allocation;
    bool ReadOnly = false;
};
struct DXGK_ALLOCATIONLIST
{
    void *hDeviceSpecificAllocation;
    UINT SegmentId = 1, Reserved = 0, WriteOperation = 1;
    struct
    {
        int64_t QuadPart = 0x2000;
    } PhysicalAddress;
};
struct VIOGPU_WDDM_ALLOCATION_REFERENCE
{
    UINT AllocationIndex = 0, Flags = 3;
    ULONGLONG AllocationOffset = 16, Length = 64;
    UINT PatchOffset = 0;
};
struct VIOGPU_WDDM_RENDER_COMMAND
{
    UINT AllocationReferenceCount, AllocationReferencesOffset, CommandStreamOffset, CommandStreamSize;
};
struct VIOGPU_WDDM_MSM_SUBMIT_BO
{
    UINT Flags, Handle;
    ULONGLONG Presumed;
};
// A pointer is used solely to model the variable payload without a compiler
// extension. The actual packet ABI has its own independent production test.
struct MSM_CCMD_GEM_SUBMIT_REQ
{
    UINT nr_bos;
    BYTE *payload;
};
struct VIOGPU_WDDM_SUBMISSION_REFERENCE : VIOGPU_WDDM_ALLOCATION_REFERENCE
{
    VIOGPU_WDDM_ALLOCATION *Allocation;
};
struct VIOGPU_WDDM_SUBMISSION
{
    struct Adapter *Adapter;
    VIOGPU_WDDM_CONTEXT *Context;
    VIOGPU_WDDM_SUBMISSION_REFERENCE *References;
    UINT AllocationCount;
    void *CommandStream;
    UINT CommandStreamSize;
    UINT ContextId = 62;
    LONG Generation = 3;
    ULONGLONG ResetGeneration = 4;
};
static int locks;
NTSTATUS AcquireAllocationLifecycle(VIOGPU_WDDM_ALLOCATION *allocation)
{
    if (!allocation)
    {
        return STATUS_INVALID_PARAMETER;
    }
    ++locks;
    ++allocation->LifecycleMutex;
    return STATUS_SUCCESS;
}
void KeReleaseMutex(int *lock, bool)
{
    --*lock;
    --locks;
}
bool IsNativeAllocation(const VIOGPU_WDDM_ALLOCATION *allocation)
{
    return allocation->Native;
}
void RtlCopyMemory(void *out, const void *in, size_t size)
{
    std::memcpy(out, in, size);
}
// INSERT_PRODUCTION

struct Fixture
{
    Adapter adapter;
    VIOGPU_WDDM_DEVICE device{&adapter};
    VIOGPU_WDDM_CONTEXT context;
    VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot{&context.NativeContext, 3, 4, 62};
    VIOGPU_WDDM_ALLOCATION allocation;
    VIOGPU_WDDM_OPEN_ALLOCATION open{VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE, &device, &allocation, false};
    DXGK_ALLOCATIONLIST entry{&open, 1, 0, 1, {0x2000}};
    alignas(8) std::array<BYTE, 256> bytes{};
    VIOGPU_WDDM_RENDER_COMMAND *header = reinterpret_cast<VIOGPU_WDDM_RENDER_COMMAND *>(bytes.data());
    VIOGPU_WDDM_ALLOCATION_REFERENCE *ref = reinterpret_cast<VIOGPU_WDDM_ALLOCATION_REFERENCE *>(bytes.data() + 16);
    MSM_CCMD_GEM_SUBMIT_REQ *request = reinterpret_cast<MSM_CCMD_GEM_SUBMIT_REQ *>(bytes.data() + 64);
    VIOGPU_WDDM_MSM_SUBMIT_BO *bo = reinterpret_cast<VIOGPU_WDDM_MSM_SUBMIT_BO *>(bytes.data() + 80);
    VIOGPU_WDDM_SUBMISSION_REFERENCE retained{};
    VIOGPU_WDDM_SUBMISSION submission{&adapter, &context, &retained, 1, request, 32};
    bool fully = false;
    Fixture()
    {
        allocation.Adapter = &adapter;
        allocation.NativeContext = &context.NativeContext;
        *header = {1, 16, 64, 32};
        *ref = {};
        ref->PatchOffset = 24;
        *request = {1, reinterpret_cast<BYTE *>(bo)};
        static_cast<VIOGPU_WDDM_ALLOCATION_REFERENCE &>(retained) = *ref;
        retained.Allocation = &allocation;
    }
    NTSTATUS render()
    {
        return ApplyRenderPrepatches(header, &device, &entry, 1, &snapshot, &fully);
    }
    NTSTATUS dispatch()
    {
        return ValidateNativeRenderBindings(&submission);
    }
    void evict()
    {
        allocation.HostState = VioGpuWddmAllocationHostNone;
        allocation.PlacementValid = false;
        allocation.PlacementOffset = 0;
        allocation.ApertureMdl = allocation.ApertureAddress = nullptr;
        allocation.BoundContextId = 0;
        allocation.BoundGeneration = 0;
        allocation.BoundResetGeneration = 0;
    }
    void resident()
    {
        allocation.HostState = VioGpuWddmAllocationHostLive;
        allocation.PlacementValid = true;
        allocation.PlacementOffset = 0x5000;
        allocation.ApertureMdl = allocation.ApertureAddress = &allocation;
        allocation.BoundContextId = 62;
        allocation.BoundGeneration = 3;
        allocation.BoundResetGeneration = 4;
    }
};
static unsigned checks, failures;
void check(bool value, const char *name)
{
    ++checks;
    if (!value || locks != 0)
    {
        ++failures;
        std::printf("FAIL %s locks=%d\n", name, locks);
    }
}
int main()
{
    {
        Fixture f;
        check(f.render() == STATUS_SUCCESS && f.fully, "resident Render");
        check(f.bo->Handle == f.allocation.ResourceId && f.bo->Presumed == 0x4400000010ULL, "translated addresses");
        check(f.dispatch() == STATUS_SUCCESS, "prepatched dispatch without Patch");
    }
    {
        Fixture f;
        f.evict();
        check(f.render() == STATUS_SUCCESS && f.fully, "evicted allocation retains a nonzero VidMm hint");
        check(f.dispatch() != STATUS_SUCCESS, "never dispatch an unbound prepatch");
        f.resident();
        check(f.dispatch() == STATUS_SUCCESS, "residency restored after Render at a new aperture offset");
    }
    {
        Fixture f;
        f.resident();
        check(f.render() == STATUS_SUCCESS && f.fully, "last-known offset differs from current offset");
        check(f.dispatch() == STATUS_SUCCESS, "stable IOVA survives relocation");
    }
    {
        Fixture f;
        f.entry.SegmentId = 0;
        check(f.render() == STATUS_SUCCESS && !f.fully && f.bo->Handle == 0, "zero hint defers translation");
        check(f.dispatch() != STATUS_SUCCESS, "unpatched payload cannot execute");
    }
    {
        Fixture f;
        VIOGPU_WDDM_ALLOCATION second = f.allocation;
        ++second.ResourceId;
        second.BlobId = second.ResourceId;
        second.PrivateData.RequestedIova += 0x10000;
        VIOGPU_WDDM_OPEN_ALLOCATION secondOpen{VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE, &f.device, &second, false};
        DXGK_ALLOCATIONLIST entries[2] = {f.entry, {&secondOpen, 1, 0, 1, {0x2000}}};
        auto *refs = f.ref;
        refs[1] = refs[0];
        refs[1].AllocationIndex = 1;
        refs[1].PatchOffset = 40;
        f.header->AllocationReferenceCount = 2;
        f.header->CommandStreamOffset = 128;
        f.header->CommandStreamSize = 48;
        auto *request = reinterpret_cast<MSM_CCMD_GEM_SUBMIT_REQ *>(f.bytes.data() + 128);
        auto *bos = reinterpret_cast<VIOGPU_WDDM_MSM_SUBMIT_BO *>(f.bytes.data() + 144);
        *request = {2, reinterpret_cast<BYTE *>(bos)};
        VIOGPU_WDDM_SUBMISSION_REFERENCE retained[2] = {};
        for (UINT i = 0; i < 2; ++i)
        {
            static_cast<VIOGPU_WDDM_ALLOCATION_REFERENCE &>(retained[i]) = refs[i];
            retained[i].Allocation = i == 0 ? &f.allocation : &second;
        }
        f.submission.References = retained;
        f.submission.AllocationCount = 2;
        f.submission.CommandStream = request;
        f.submission.CommandStreamSize = 48;
        check(ApplyRenderPrepatches(f.header, &f.device, entries, 2, &f.snapshot, &f.fully) == STATUS_SUCCESS,
              "multiple allocation translations");
        check(f.dispatch() == STATUS_SUCCESS, "all bindings accepted");
        ++bos[1].Presumed;
        check(f.dispatch() != STATUS_SUCCESS, "second reference address corruption rejected");
        --bos[1].Presumed;
        second.HostState = VioGpuWddmAllocationHostNone;
        check(f.dispatch() != STATUS_SUCCESS, "second reference eviction rejected");
    }
    // Translation still enforces lifetime identity, range, access and segment.
    for (unsigned test = 0; test < 9; ++test)
    {
        Fixture f;
        switch (test)
        {
            case 0:
                f.allocation.Destroying = true;
                break;
            case 1:
                ++f.allocation.ContextGeneration;
                break;
            case 2:
                ++f.allocation.ContextResetGeneration;
                break;
            case 3:
                f.allocation.NativeContext = nullptr;
                break;
            case 4:
                f.open.ReadOnly = true;
                break;
            case 5:
                f.entry.SegmentId = 2;
                break;
            case 6:
                f.ref->AllocationOffset = MAXULONGLONG;
                break;
            case 7:
                f.allocation.PrivateData.RequestedIova = MAXULONGLONG;
                break;
            case 8:
                f.allocation.BlobId = 0;
                break;
        }
        check(f.render() != STATUS_SUCCESS, "invalid translation is rejected");
    }
    // State can change after Render. Validate the final data, not the old hint.
    for (unsigned test = 0; test < 14; ++test)
    {
        Fixture f;
        check(f.render() == STATUS_SUCCESS, "final-binding test preparation");
        switch (test)
        {
            case 0:
                f.evict();
                break;
            case 1:
                ++f.allocation.BoundContextId;
                break;
            case 2:
                ++f.allocation.BoundGeneration;
                break;
            case 3:
                ++f.allocation.BoundResetGeneration;
                break;
            case 4:
                ++f.bo->Handle;
                break;
            case 5:
                ++f.bo->Presumed;
                break;
            case 6:
                ++f.allocation.PrivateData.ExpectedResetGeneration;
                break;
            case 7:
                f.allocation.ApertureMdl = nullptr;
                break;
            case 8:
                f.allocation.ApertureAddress = nullptr;
                break;
            case 9:
                f.retained.PatchOffset = MAXUINT;
                break;
            case 10:
                f.retained.Allocation = nullptr;
                break;
            case 11:
                f.request->nr_bos = 2;
                break;
            case 12:
                f.submission.CommandStreamSize = 1;
                break;
            case 13:
                f.allocation.Native = false;
                break;
        }
        check(f.dispatch() != STATUS_SUCCESS, "invalid final binding is rejected");
    }
    std::printf("Render prepatch: %u/%u PASS\n", checks - failures, checks);
    return failures ? 1 : 0;
}
