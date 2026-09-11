#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define _Use_decl_annotations_
#define APIENTRY
#define CONST const
#define TRUE true
#define FALSE false
using VOID = void;
using HANDLE = void *;
using BYTE = unsigned char;
using UINT = unsigned int;
using SIZE_T = size_t;
using ULONGLONG = uint64_t;
using BOOLEAN = bool;
using NTSTATUS = int;
constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS STATUS_INVALID_PARAMETER = -1;
constexpr NTSTATUS STATUS_BUFFER_TOO_SMALL = -2;
constexpr NTSTATUS STATUS_DEVICE_NOT_READY = -3;
constexpr UINT PAGE_SIZE = 4096;
constexpr SIZE_T VIOGPU_WDDM_APERTURE_SIZE = 512ULL << 20;
constexpr UINT VIOGPU_WDDM_SEGMENT_ID = 1;
constexpr UINT VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE = 1;
constexpr UINT D3DDDI_ALLOCATIONPRIORITY_NORMAL = 0x78000000;
constexpr UINT DXGK_ENGINE_TYPE_3D = 1;
void RtlZeroMemory(void *address, size_t size) { std::memset(address, 0, size); }
void RtlCopyMemory(void *target, const void *source, size_t size) { std::memcpy(target, source, size); }
struct GPU_CAPSET_DRM {};
struct VIOGPU_WDDM_PAGING_PRIVATE { BYTE bytes[256]; };
struct DXGKARG_QUERYADAPTERINFO { const void *pInputData; UINT InputDataSize; void *pOutputData; UINT OutputDataSize; };
struct DXGK_QUERYSEGMENTIN4 { UINT PhysicalAdapterIndex; };
struct SegmentFlags { UINT CpuVisible; UINT Aperture; UINT CacheCoherent; UINT Unimplemented; };
struct DXGK_SEGMENTDESCRIPTOR4 { SegmentFlags Flags; uint64_t BaseAddress; SIZE_T Size; SIZE_T CommitLimit; uint64_t OtherFields[10]; };
struct DXGK_QUERYSEGMENTOUT4 {
    UINT NbSegment; BYTE *pSegmentDescriptor; UINT PagingBufferSegmentId;
    UINT PagingBufferSize; UINT PagingBufferPrivateDataSize; SIZE_T SegmentDescriptorStride;
};
struct DXGK_PHYSICALADAPTERCAPS { UINT NumExecutionNodes; UINT PagingNodeIndex; HANDLE DxgkPhysicalAdapterHandle; UINT Flags; };
struct DXGKARG_GETNODEMETADATA { UINT EngineType; uint16_t FriendlyName[32]; UINT Flags; UINT Reserved; BOOLEAN GpuMmuSupported; BOOLEAN IoMmuSupported; };
struct Interface { HANDLE DeviceHandle; };
struct VioGpuDod {
    bool Ready = true; uint64_t Generation = 1; Interface Iface{reinterpret_cast<HANDLE>(0x1234)};
    bool QueryNativeContextReadiness(GPU_CAPSET_DRM *, void *, void *, ULONGLONG *generation) { *generation = Generation; return Ready; }
    Interface *GetDxgkInterface() { return &Iface; }
};
union AllocationFlags {
    struct { UINT CpuVisible : 1; UINT PermanentSysMem : 1; UINT Cached : 1; UINT ReservedLow : 12; UINT AccessedPhysically : 1; UINT ReservedHigh : 16; };
    UINT Value;
};
struct VIOGPU_WDDM_ALLOCATION { UINT Flags; };
struct DXGK_ALLOCATIONINFO {
    UINT Alignment; SIZE_T Size; SIZE_T PitchAlignedSize; struct { UINT Value; } HintedBank;
    struct { UINT Value; UINT SegmentId0; UINT Direction0; } PreferredSegment;
    UINT SupportedReadSegmentSet; UINT SupportedWriteSegmentSet; UINT EvictionSegmentSet;
    UINT PhysicalAdapterIndex; AllocationFlags FlagsWddm2; void *pAllocationUsageHint; UINT AllocationPriority; HANDLE hAllocation;
};

// INSERT_PRODUCTION

unsigned checks = 0;
void check(bool condition, const char *message) { ++checks; if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }

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
    check(QuerySegment4(&adapter, &query) == STATUS_SUCCESS && !std::memcmp(&output, &expected, sizeof(output)), "count query touches only NbSegment, ignores poison descriptor");
    std::array<BYTE, sizeof(DXGK_SEGMENTDESCRIPTOR4) + 32> storage;
    storage.fill(0xa5);
    output.pSegmentDescriptor = storage.data();
    output.SegmentDescriptorStride = storage.size();
    check(QuerySegment4(&adapter, &query) == STATUS_SUCCESS, "versioned descriptor with larger stride accepted");
    DXGK_SEGMENTDESCRIPTOR4 descriptor;
    std::memcpy(&descriptor, storage.data(), sizeof(descriptor));
    check(descriptor.Size == VIOGPU_WDDM_APERTURE_SIZE && descriptor.CommitLimit == VIOGPU_WDDM_APERTURE_SIZE && descriptor.Flags.Aperture && descriptor.Flags.CpuVisible && descriptor.Flags.CacheCoherent, "single shared system-memory aperture");
    bool tail = true;
    for (size_t i = sizeof(descriptor); i < storage.size(); ++i) tail &= storage[i] == 0xa5;
    check(tail && descriptor.BaseAddress == 0 && !descriptor.Flags.Unimplemented, "no descriptor overrun or invented VRAM/capabilities");
    check(output.PagingBufferSegmentId == 0 && output.PagingBufferSize == PAGE_SIZE && output.PagingBufferPrivateDataSize == sizeof(VIOGPU_WDDM_PAGING_PRIVATE), "physical paging buffers preserve existing transaction storage");
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
    DXGK_PHYSICALADAPTERCAPS caps;
    std::memset(&caps, 0xa5, sizeof(caps));
    query.pOutputData = &caps; query.OutputDataSize = sizeof(caps);
    check(QueryPhysicalAdapterCaps(&adapter, &query) == STATUS_SUCCESS && caps.NumExecutionNodes == 1 && caps.PagingNodeIndex == 0 && caps.DxgkPhysicalAdapterHandle == adapter.Iface.DeviceHandle && caps.Flags == 0, "real adapter handle, single paging node, no MMU fiction");
    DXGKARG_GETNODEMETADATA metadata;
    std::memset(&metadata, 0xa5, sizeof(metadata));
    check(VioGpuWddmGetNodeMetadata(&adapter, 0, &metadata) == STATUS_SUCCESS && metadata.EngineType == DXGK_ENGINE_TYPE_3D && !metadata.GpuMmuSupported && !metadata.IoMmuSupported && !metadata.Flags && !metadata.Reserved && !metadata.FriendlyName[0], "single physical-mode 3D engine");
    check(VioGpuWddmGetNodeMetadata(&adapter, 1, &metadata) == STATUS_INVALID_PARAMETER && VioGpuWddmGetNodeMetadata(&adapter, 0x10000, &metadata) == STATUS_INVALID_PARAMETER && VioGpuWddmGetNodeMetadata(nullptr, 0, &metadata) == STATUS_INVALID_PARAMETER && VioGpuWddmGetNodeMetadata(&adapter, 0, nullptr) == STATUS_INVALID_PARAMETER, "node and adapter ordinals/null outputs validated");
    VIOGPU_WDDM_ALLOCATION allocation{VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE};
    DXGK_ALLOCATIONINFO allocInfo;
    std::memset(&allocInfo, 0xa5, sizeof(allocInfo));
    InitializeAllocationInfo(&allocInfo, &allocation, 8192);
    check(allocInfo.FlagsWddm2.CpuVisible && allocInfo.FlagsWddm2.Cached && allocInfo.FlagsWddm2.AccessedPhysically && !allocInfo.FlagsWddm2.ReservedLow && !allocInfo.FlagsWddm2.ReservedHigh, "physical allocation uses WDDM2 flags without legacy synchronous bit");
    check(allocInfo.PhysicalAdapterIndex == 0 && allocInfo.Size == 8192 && allocInfo.SupportedWriteSegmentSet == 1 && allocInfo.SupportedReadSegmentSet == 1 && allocInfo.hAllocation == &allocation, "allocation identity and residency mask preserved");
    allocation.Flags = 0;
    InitializeAllocationInfo(&allocInfo, &allocation, 16384);
    check(!allocInfo.FlagsWddm2.CpuVisible && !allocInfo.FlagsWddm2.Cached && allocInfo.FlagsWddm2.AccessedPhysically, "GPU-only allocation still participates in physical residency");
    std::printf("PASS: %u production WDDM2 accounting cases\n", checks);
}
