#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>

#ifndef _Use_decl_annotations_
#define _Use_decl_annotations_
#endif
#define APIENTRY
#define CONST const
#define TRUE  true
#define FALSE false
using VOID = void;
using HANDLE = void *;
using BYTE = unsigned char;
using UINT = unsigned int;
using SIZE_T = size_t;
using ULONGLONG = uint64_t;
using UINT32 = uint32_t;
using UINT64 = uint64_t;
using BOOLEAN = bool;
using NTSTATUS = int;
constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS STATUS_INVALID_PARAMETER = -1;
constexpr NTSTATUS STATUS_BUFFER_TOO_SMALL = -2;
constexpr NTSTATUS STATUS_DEVICE_NOT_READY = -3;
constexpr UINT PAGE_SIZE = 4096;
#define FIELD_OFFSET(type, field) offsetof(type, field)
constexpr SIZE_T VIOGPU_WDDM_APERTURE_SIZE = 512ULL << 20;
constexpr UINT VIOGPU_WDDM_SEGMENT_ID = 1;
constexpr UINT VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE = 1;
constexpr UINT D3DDDI_ALLOCATIONPRIORITY_NORMAL = 0x78000000;
constexpr UINT DXGK_ENGINE_TYPE_3D = 1;
void RtlZeroMemory(void *address, size_t size)
{
    std::memset(address, 0, size);
}
void RtlCopyMemory(void *target, const void *source, size_t size)
{
    std::memcpy(target, source, size);
}
struct GPU_CAPSET_DRM
{
};
struct VIOGPU_WDDM_PAGING_PRIVATE
{
    BYTE bytes[256];
};
struct DXGKARG_QUERYADAPTERINFO
{
    const void *pInputData;
    UINT InputDataSize;
    void *pOutputData;
    UINT OutputDataSize;
};
struct DXGK_QUERYSEGMENTIN4
{
    UINT PhysicalAdapterIndex;
};
struct SegmentFlags
{
    UINT CpuVisible;
    UINT Aperture;
    UINT CacheCoherent;
    UINT Unimplemented;
};
struct DXGK_SEGMENTDESCRIPTOR4
{
    SegmentFlags Flags;
    uint64_t BaseAddress;
    SIZE_T Size;
    SIZE_T CommitLimit;
    uint64_t OtherFields[10];
};
struct DXGK_QUERYSEGMENTOUT4
{
    UINT NbSegment;
    BYTE *pSegmentDescriptor;
    UINT PagingBufferSegmentId;
    UINT PagingBufferSize;
    UINT PagingBufferPrivateDataSize;
    SIZE_T SegmentDescriptorStride;
};
struct DXGKARG_GETNODEMETADATA
{
    UINT EngineType;
    uint16_t FriendlyName[32];
    UINT Flags;
    UINT Reserved;
    BOOLEAN GpuMmuSupported;
    BOOLEAN IoMmuSupported;
};
struct DXGK_QUERYPHYSICALADAPTERCAPSIN
{
    UINT PhysicalAdapterIndex;
};
struct DXGK_PHYSICALADAPTERCAPS
{
    UINT NumExecutionNodes;
    UINT PagingNodeIndex;
    HANDLE DxgkPhysicalAdapterHandle;
    union {
        UINT Value;
    } Flags;
    UINT VPRPagingNode;
};
struct LARGE_INTEGER
{
    int64_t QuadPart;
};
uint64_t performanceSamples;
LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER *frequency)
{
    if (frequency != nullptr)
    {
        frequency->QuadPart = 1000000;
    }
    return LARGE_INTEGER{static_cast<int64_t>(0x100000000ULL + 7919 * ++performanceSamples)};
}
struct DXGKARG_CALIBRATEGPUCLOCK
{
    uint64_t GpuClockCounter;
    uint64_t CpuClockCounter;
};
struct DXGKARG_HISTORYBUFFERPRECISION
{
    uint32_t PrecisionBits;
};
struct DXGK_DISPLAY_DRIVERCAPS_EXTENSION
{
    union {
        UINT Value;
    };
};
struct DXGKRNL_INTERFACE
{
    UINT Size;
    HANDLE DeviceHandle;
};
struct VioGpuDod
{
    DXGKRNL_INTERFACE Interface{sizeof(DXGKRNL_INTERFACE), reinterpret_cast<HANDLE>(0x1234)};
    DXGKRNL_INTERFACE *GetDxgkInterface()
    {
        return &Interface;
    }
    bool Ready = true;
    uint64_t Generation = 1;
    bool QueryNativeContextReadiness(GPU_CAPSET_DRM *, void *, void *, ULONGLONG *generation)
    {
        *generation = Generation;
        return Ready;
    }
};
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4201) // WDK flag structures use anonymous bitfields.
#endif
union AllocationFlags {
    struct
    {
        UINT CpuVisible : 1;
        UINT PermanentSysMem : 1;
        UINT Cached : 1;
        UINT ReservedLow : 12;
        UINT AccessedPhysically : 1;
        UINT ReservedHigh : 16;
    };
    UINT Value;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
struct VIOGPU_WDDM_ALLOCATION
{
    UINT Flags;
};
struct DXGK_ALLOCATIONINFO
{
    UINT Alignment;
    SIZE_T Size;
    SIZE_T PitchAlignedSize;
    struct
    {
        UINT Value;
    } HintedBank;
    struct
    {
        UINT Value;
        UINT SegmentId0;
        UINT Direction0;
    } PreferredSegment;
    UINT SupportedReadSegmentSet;
    UINT SupportedWriteSegmentSet;
    UINT EvictionSegmentSet;
    UINT PhysicalAdapterIndex;
    AllocationFlags FlagsWddm2;
    void *pAllocationUsageHint;
    UINT AllocationPriority;
    HANDLE hAllocation;
};

// INSERT_PRODUCTION

unsigned checks = 0;
void check(bool condition, const char *message)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main()
{
    VioGpuDod adapter;
    DXGK_QUERYSEGMENTIN4 input{};
    DXGK_QUERYSEGMENTOUT4 output;
    std::memset(&output, 0xa5, sizeof(output));
    output.NbSegment = 0;
    auto expected = output;
    expected.NbSegment = 1;
    DXGKARG_QUERYADAPTERINFO query{&input, sizeof(input), &output, sizeof(output)};
    check(QuerySegment4(&adapter, &query) == STATUS_SUCCESS && !std::memcmp(&output, &expected, sizeof(output)),
          "count query touches only NbSegment, ignores poison descriptor");
    std::array<BYTE, sizeof(DXGK_SEGMENTDESCRIPTOR4) + 32> storage;
    storage.fill(0xa5);
    output.pSegmentDescriptor = storage.data();
    output.SegmentDescriptorStride = storage.size();
    check(QuerySegment4(&adapter, &query) == STATUS_SUCCESS, "versioned descriptor with larger stride accepted");
    DXGK_SEGMENTDESCRIPTOR4 descriptor;
    std::memcpy(&descriptor, storage.data(), sizeof(descriptor));
    check(descriptor.Size == VIOGPU_WDDM_APERTURE_SIZE && descriptor.CommitLimit == VIOGPU_WDDM_APERTURE_SIZE &&
                                                                                                              descriptor.Flags.Aperture &&
                                                                                                              descriptor.Flags.CpuVisible &&
                                                                                                              descriptor.Flags.CacheCoherent,
          "single shared system-memory aperture");
    bool tail = true;
    for (size_t i = sizeof(descriptor); i < storage.size(); ++i)
    {
        tail &= storage[i] == 0xa5;
    }
    check(tail && descriptor.BaseAddress == 0 && !descriptor.Flags.Unimplemented,
          "no descriptor overrun or invented VRAM/capabilities");
    check(output.PagingBufferSegmentId == 0 && output.PagingBufferSize == PAGE_SIZE && output.PagingBufferPrivateDataSize == sizeof(VIOGPU_WDDM_PAGING_PRIVATE),
          "physical paging buffers preserve existing transaction storage");
    output.SegmentDescriptorStride = sizeof(descriptor) - 1;
    check(QuerySegment4(&adapter, &query) == STATUS_INVALID_PARAMETER, "short stride rejected");
    output.SegmentDescriptorStride = sizeof(descriptor);
    output.NbSegment = 2;
    check(QuerySegment4(&adapter, &query) == STATUS_INVALID_PARAMETER, "wrong segment count rejected");
    output.NbSegment = 1;
    input.PhysicalAdapterIndex = 1;
    check(QuerySegment4(&adapter, &query) == STATUS_INVALID_PARAMETER, "no invented linked physical adapter");
    input.PhysicalAdapterIndex = 0;
    query.InputDataSize = sizeof(input) - 1;
    check(QuerySegment4(&adapter, &query) == STATUS_INVALID_PARAMETER, "short input rejected");
    query.InputDataSize = sizeof(input);
    query.OutputDataSize = sizeof(output) - 1;
    check(QuerySegment4(&adapter, &query) == STATUS_BUFFER_TOO_SMALL, "short output rejected");
    query.OutputDataSize = sizeof(output);
    adapter.Generation = 0;
    check(QuerySegment4(&adapter, &query) == STATUS_DEVICE_NOT_READY, "unpublished reset generation rejected");
    adapter.Generation = 1;
    adapter.Ready = false;
    check(QuerySegment4(&adapter, &query) == STATUS_DEVICE_NOT_READY, "unready host rejected");
    adapter.Ready = true;
    DXGKARG_GETNODEMETADATA metadata;
    std::memset(&metadata, 0xa5, sizeof(metadata));
    check(VioGpuWddmGetNodeMetadata(&adapter,
                                    0,
                                    &metadata) == STATUS_SUCCESS && metadata.EngineType == DXGK_ENGINE_TYPE_3D &&
                                                                                                              !metadata.GpuMmuSupported &&
                                                                                                              !metadata.IoMmuSupported &&
                                                                                                              !metadata.Flags &&
                                                                                                              !metadata.Reserved &&
                                                                                                              !metadata.FriendlyName[0],
          "single physical-mode 3D engine");
    check(VioGpuWddmGetNodeMetadata(&adapter,
                                    1,
                                    &metadata) == STATUS_INVALID_PARAMETER && VioGpuWddmGetNodeMetadata(&adapter, 0x10000, &metadata) == STATUS_INVALID_PARAMETER &&
                                                                                                              VioGpuWddmGetNodeMetadata(nullptr,
                                                                                                                                        0,
                                                                                                                                        &metadata) == STATUS_INVALID_PARAMETER &&
                                                                                                              VioGpuWddmGetNodeMetadata(&adapter,
                                                                                                                                        0,
                                                                                                                                        nullptr) == STATUS_INVALID_PARAMETER,
          "node and adapter ordinals/null outputs validated");
    VIOGPU_WDDM_ALLOCATION allocation{VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE};
    DXGK_ALLOCATIONINFO allocInfo;
    std::memset(&allocInfo, 0xa5, sizeof(allocInfo));
    InitializeAllocationInfo(&allocInfo, &allocation, 8192);
    check(allocInfo.FlagsWddm2.CpuVisible && allocInfo.FlagsWddm2.Cached && allocInfo.FlagsWddm2.AccessedPhysically &&
                                                                                                              !allocInfo.FlagsWddm2.ReservedLow &&
                                                                                                              !allocInfo.FlagsWddm2.ReservedHigh,
          "physical allocation uses WDDM2 flags without legacy synchronous bit");
    check(allocInfo.PhysicalAdapterIndex == 0 && allocInfo.Size == 8192 && allocInfo.SupportedWriteSegmentSet == 1 &&
                                                                                                              allocInfo.SupportedReadSegmentSet == 1 &&
                                                                                                              allocInfo.hAllocation == &allocation,
          "allocation identity and residency mask preserved");
    allocation.Flags = 0;
    InitializeAllocationInfo(&allocInfo, &allocation, 16384);
    check(!allocInfo.FlagsWddm2.CpuVisible && !allocInfo.FlagsWddm2.Cached && allocInfo.FlagsWddm2.AccessedPhysically,
          "GPU-only allocation still participates in physical residency");
        {
        DXGK_QUERYPHYSICALADAPTERCAPSIN physicalIn{};
        std::array<unsigned char, sizeof(DXGK_PHYSICALADAPTERCAPS) + 8> caps;
        const UINT prefix = static_cast<UINT>(offsetof(DXGK_PHYSICALADAPTERCAPS, Flags) + sizeof(UINT));
        caps.fill(0xa5);
        DXGKARG_QUERYADAPTERINFO physical{&physicalIn, sizeof(physicalIn), caps.data(), prefix};
        check(QueryPhysicalAdapterCaps(&adapter, &physical) == STATUS_SUCCESS, "OS WDDM2.0 20-byte extent accepted");
        DXGK_PHYSICALADAPTERCAPS decoded{};
        std::memcpy(&decoded, caps.data(), prefix);
        check(decoded.NumExecutionNodes == 1 && decoded.PagingNodeIndex == 0 &&
                  decoded.DxgkPhysicalAdapterHandle == adapter.Interface.DeviceHandle && decoded.Flags.Value == 0,
              "one physical node pages itself with the actual DXGK handle and no MMU flags");
        for (size_t i = prefix; i < caps.size(); ++i)
        {
            check(caps[i] == 0xa5, "bytes beyond the supplied extent untouched");
        }
        physical.OutputDataSize = static_cast<UINT>(caps.size());
        caps.fill(0xa5);
        check(QueryPhysicalAdapterCaps(&adapter, &physical) == STATUS_SUCCESS, "larger future extent accepted");
        for (size_t i = prefix; i < caps.size(); ++i)
        {
            check(caps[i] == 0, "unimplemented later fields zeroed");
        }
        physical.OutputDataSize = prefix - 1;
        check(QueryPhysicalAdapterCaps(&adapter, &physical) == STATUS_BUFFER_TOO_SMALL, "short extent rejected");
        physical.OutputDataSize = prefix;
        physicalIn.PhysicalAdapterIndex = 1;
        check(QueryPhysicalAdapterCaps(&adapter, &physical) == STATUS_INVALID_PARAMETER, "no linked adapter invented");
        physicalIn.PhysicalAdapterIndex = 0;
        physical.InputDataSize = sizeof(physicalIn) - 1;
        check(QueryPhysicalAdapterCaps(&adapter, &physical) == STATUS_INVALID_PARAMETER, "short input rejected");
        physical.InputDataSize = sizeof(physicalIn);
        physical.pInputData = nullptr;
        check(QueryPhysicalAdapterCaps(&adapter, &physical) == STATUS_INVALID_PARAMETER, "missing input rejected");
        physical.pInputData = &physicalIn;
        physical.pOutputData = nullptr;
        check(QueryPhysicalAdapterCaps(&adapter, &physical) == STATUS_BUFFER_TOO_SMALL, "missing output rejected");
    }
    {
        std::array<unsigned char, 12> precision;
        precision.fill(0xa5);
        DXGKARG_QUERYADAPTERINFO history{nullptr, 4, precision.data(), 4};
        check(QueryHistoryBufferPrecision(&history) == STATUS_SUCCESS, "one-node history precision answered");
        DXGKARG_HISTORYBUFFERPRECISION node{};
        std::memcpy(&node, precision.data(), sizeof(node));
        check(node.PrecisionBits == 64, "64-bit timestamps, never the FormatHistoryBuffer contract");
        check(precision[4] == 0xa5 && precision[11] == 0xa5, "bytes beyond one node untouched");
        history.OutputDataSize = 12;
        check(QueryHistoryBufferPrecision(&history) == STATUS_SUCCESS, "every node answered");
        for (UINT offset = 0; offset < 12; offset += 4)
        {
            std::memcpy(&node, precision.data() + offset, sizeof(node));
            check(node.PrecisionBits == 64, "each node precision is valid (32..64)");
        }
        history.OutputDataSize = 6;
        check(QueryHistoryBufferPrecision(&history) == STATUS_INVALID_PARAMETER, "partial node rejected");
        history.OutputDataSize = 3;
        check(QueryHistoryBufferPrecision(&history) == STATUS_INVALID_PARAMETER, "short output rejected");
        history.OutputDataSize = 4;
        history.pOutputData = nullptr;
        check(QueryHistoryBufferPrecision(&history) == STATUS_INVALID_PARAMETER, "missing output rejected");

        DXGKARG_CALIBRATEGPUCLOCK clock{0xdeadbeef, 0xfeedface};
        performanceSamples = 0;
        check(VioGpuWddmCalibrateGpuClock(&adapter, 0, 0, &clock) == STATUS_SUCCESS, "clock calibration answered");
        check(performanceSamples == 1 && clock.GpuClockCounter == clock.CpuClockCounter &&
                  clock.CpuClockCounter == 0x100000000ULL + 7919,
              "GPU and CPU counters come from one performance counter sample");
        check(VioGpuWddmCalibrateGpuClock(&adapter, 1, 0, &clock) == STATUS_INVALID_PARAMETER, "unknown node rejected");
        check(VioGpuWddmCalibrateGpuClock(&adapter, 0, 1, &clock) == STATUS_INVALID_PARAMETER, "unknown engine rejected");
        check(VioGpuWddmCalibrateGpuClock(nullptr, 0, 0, &clock) == STATUS_INVALID_PARAMETER, "missing adapter rejected");
        check(VioGpuWddmCalibrateGpuClock(&adapter, 0, 0, nullptr) == STATUS_INVALID_PARAMETER, "missing output rejected");
        check(performanceSamples == 1, "refused calibrations take no sample");

        std::array<unsigned char, 8> extension;
        extension.fill(0xa5);
        DXGKARG_QUERYADAPTERINFO display{nullptr, 0, extension.data(), 4};
        check(QueryDisplayDriverCapsExtension(&display) == STATUS_SUCCESS, "display caps extension answered");
        check(extension[0] == 0 && extension[1] == 0 && extension[2] == 0 && extension[3] == 0,
              "no secure display or virtual mode support claimed");
        check(extension[4] == 0xa5, "bytes beyond the extension untouched");
        display.OutputDataSize = 3;
        check(QueryDisplayDriverCapsExtension(&display) == STATUS_INVALID_PARAMETER, "short extension rejected");
        display.OutputDataSize = 4;
        display.pOutputData = nullptr;
        check(QueryDisplayDriverCapsExtension(&display) == STATUS_INVALID_PARAMETER, "missing extension rejected");
    }
    std::printf("PASS: %u production WDDM2 accounting cases\n", checks);
}
