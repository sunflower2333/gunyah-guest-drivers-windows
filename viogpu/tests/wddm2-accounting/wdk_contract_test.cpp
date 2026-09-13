// Actual WDK layout and flag unions; only hardware readiness is a controlled peer.
#include <ntddk.h>
#include <windef.h>
#include <d3dkmddi.h>
#include <dispmprt.h> // DXGKRNL_INTERFACE
#include "../../common/mmio_flip.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>

constexpr SIZE_T VIOGPU_WDDM_APERTURE_SIZE = 512ULL << 20;
constexpr UINT VIOGPU_WDDM_SEGMENT_ID = 1;
constexpr UINT VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE = 1;
struct GPU_CAPSET_DRM
{
};
struct VIOGPU_WDDM_PAGING_PRIVATE
{
    unsigned char Bytes[256];
};
struct VIOGPU_WDDM_ALLOCATION
{
    UINT Flags;
};
struct VioGpuDod
{
    DXGKRNL_INTERFACE Interface = {};
    PDXGKRNL_INTERFACE GetDxgkInterface()
    {
        return &Interface;
    }
    bool Ready = true;
    bool QueryNativeContextReadiness(GPU_CAPSET_DRM *, void *, void *, ULONGLONG *generation)
    {
        *generation = Ready ? 1 : 0;
        return Ready;
    }
};
DXGKDDI_GETNODEMETADATA VioGpuWddmGetNodeMetadata;

// INSERT_PRODUCTION

unsigned checks;
void check(bool ok, const char *message)
{
    ++checks;
    if (!ok)
    {
        std::fprintf(stderr, "FAIL WDK: %s\n", message);
        std::exit(1);
    }
}
int main()
{
    // The WDK-free MMIO flip policy uses the real bit-field positions.
    DXGK_SETVIDPNSOURCEADDRESS_FLAGS sourceFlags = {};
    sourceFlags.ModeChange = 1;
    check(sourceFlags.Value == VioGpuSourceAddressFlagModeChange, "SetVidPnSourceAddress ModeChange bit");
    sourceFlags.Value = 0;
    sourceFlags.FlipImmediate = 1;
    check(sourceFlags.Value == VioGpuSourceAddressFlagFlipImmediate, "SetVidPnSourceAddress FlipImmediate bit");
    sourceFlags.Value = 0;
    sourceFlags.FlipOnNextVSync = 1;
    check(sourceFlags.Value == VioGpuSourceAddressFlagFlipOnNextVSync, "SetVidPnSourceAddress FlipOnNextVSync bit");
    DXGK_PRESENTFLAGS presentFlags = {};
    presentFlags.Blt = 1;
    check(presentFlags.Value == VioGpuPresentFlagBlt, "Present Blt bit");
    presentFlags.Value = 0;
    presentFlags.ColorFill = 1;
    check(presentFlags.Value == VioGpuPresentFlagColorFill, "Present ColorFill bit");
    presentFlags.Value = 0;
    presentFlags.Flip = 1;
    check(presentFlags.Value == VioGpuPresentFlagFlip, "Present Flip bit");
    DXGK_FLIPCAPS flipCaps = {};
    flipCaps.FlipOnVSyncMmIo = 1;
    check(flipCaps.Value == VioGpuFlipCapsOnVSyncMmIo, "FlipCaps FlipOnVSyncMmIo bit");
    check(FIELD_OFFSET(DXGK_DRIVERCAPS, FlipCaps) < FIELD_OFFSET(DXGK_DRIVERCAPS, PreemptionCaps),
          "FlipCaps lies inside the Win7 DriverCaps prefix every reply writes");

    VioGpuDod adapter;
    DXGK_QUERYSEGMENTIN4 input = {};
    DXGK_QUERYSEGMENTOUT4 output;
    std::memset(&output, 0xa5, sizeof(output));
    output.NbSegment = 0;
    auto expected = output;
    expected.NbSegment = 1;
    DXGKARG_QUERYADAPTERINFO query = {};
    query.pInputData = &input;
    query.InputDataSize = sizeof(input);
    query.pOutputData = &output;
    query.OutputDataSize = sizeof(output);
    check(QuerySegment4(&adapter, &query) == STATUS_SUCCESS && std::memcmp(&output, &expected, sizeof(output)) == 0,
          "count query writes only NbSegment using exact WDK layout");
    std::array<unsigned char, sizeof(DXGK_SEGMENTDESCRIPTOR4) + 32> storage;
    storage.fill(0xa5);
    output.pSegmentDescriptor = storage.data();
    output.SegmentDescriptorStride = static_cast<UINT>(storage.size());
    check(QuerySegment4(&adapter, &query) == STATUS_SUCCESS, "WDK segment4 descriptor accepted");
    DXGK_SEGMENTDESCRIPTOR4 descriptor;
    std::memcpy(&descriptor, storage.data(), sizeof(descriptor));
    check(descriptor.Size == VIOGPU_WDDM_APERTURE_SIZE && descriptor.Flags.CpuVisible && descriptor.Flags.Aperture &&
                                                                                                              descriptor.Flags.CacheCoherent,
          "actual WDK descriptor flags describe aperture system memory");
    for (size_t i = sizeof(descriptor); i < storage.size(); ++i)
    {
        check(storage[i] == 0xa5, "larger descriptor tail preserved");
    }
    output.SegmentDescriptorStride = sizeof(descriptor) - 1;
    check(QuerySegment4(&adapter, &query) == STATUS_INVALID_PARAMETER, "short stride rejected");
    output.SegmentDescriptorStride = sizeof(descriptor);
    input.PhysicalAdapterIndex = 1;
    check(QuerySegment4(&adapter, &query) == STATUS_INVALID_PARAMETER, "unavailable physical adapter rejected");
    input.PhysicalAdapterIndex = 0;
    adapter.Ready = false;
    check(QuerySegment4(&adapter, &query) == STATUS_DEVICE_NOT_READY, "retired readiness rejected");
    DXGKARG_GETNODEMETADATA metadata;
    std::memset(&metadata, 0xa5, sizeof(metadata));
    check(VioGpuWddmGetNodeMetadata(&adapter,
                                    0,
                                    &metadata) == STATUS_SUCCESS && metadata.EngineType == DXGK_ENGINE_TYPE_3D &&
                                                                                                              !metadata.GpuMmuSupported &&
                                                                                                              !metadata.IoMmuSupported,
          "typed WDK callback reports actual physical node");
    check(VioGpuWddmGetNodeMetadata(&adapter, 0x10000, &metadata) == STATUS_INVALID_PARAMETER,
          "combined adapter ordinal validated");
    VIOGPU_WDDM_ALLOCATION allocation{VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE};
    DXGK_ALLOCATIONINFO info;
    std::memset(&info, 0xa5, sizeof(info));
    InitializeAllocationInfo(&info, &allocation, 8192);
    check(info.FlagsWddm2.Value == (0x8000U | 1U |
                                    4U) && info.PhysicalAdapterIndex == 0 && info.hAllocation == &allocation,
          "actual allocation flag union has AccessedPhysically and no reserved legacy paging bit");
        {
        adapter.Ready = true;
        adapter.Interface.DeviceHandle = reinterpret_cast<HANDLE>(0x5678);
        DXGK_QUERYPHYSICALADAPTERCAPSIN physicalIn = {};
        std::array<unsigned char, sizeof(DXGK_PHYSICALADAPTERCAPS) + 8> caps;
        const UINT prefix = static_cast<UINT>(FIELD_OFFSET(DXGK_PHYSICALADAPTERCAPS, Flags) +
                                              sizeof(static_cast<DXGK_PHYSICALADAPTERCAPS *>(nullptr)->Flags));
        check(prefix == 20, "actual WDK WDDM2.0 physical adapter caps prefix matches the observed OS extent");
        caps.fill(0xa5);
        DXGKARG_QUERYADAPTERINFO physical = {};
        physical.Type = DXGKQAITYPE_PHYSICALADAPTERCAPS;
        physical.pInputData = &physicalIn;
        physical.InputDataSize = sizeof(physicalIn);
        physical.pOutputData = caps.data();
        physical.OutputDataSize = prefix;
        check(QueryPhysicalAdapterCaps(&adapter, &physical) == STATUS_SUCCESS, "WDK physical adapter caps accepted");
        DXGK_PHYSICALADAPTERCAPS decoded = {};
        std::memcpy(&decoded, caps.data(), prefix);
        check(decoded.NumExecutionNodes == 1 && decoded.PagingNodeIndex == 0 &&
                  decoded.DxgkPhysicalAdapterHandle == adapter.Interface.DeviceHandle && decoded.Flags.Value == 0,
              "actual WDK caps: one self-paging node, real handle, no MMU/move-paging flags");
        for (size_t i = prefix; i < caps.size(); ++i)
        {
            check(caps[i] == 0xa5, "WDK bytes beyond extent untouched");
        }
        physicalIn.PhysicalAdapterIndex = 1;
        check(QueryPhysicalAdapterCaps(&adapter, &physical) == STATUS_INVALID_PARAMETER, "WDK linked index rejected");
    }
    std::printf("PASS: %u actual WDK WDDM2 ABI and production contract assertions\n", checks);
}
