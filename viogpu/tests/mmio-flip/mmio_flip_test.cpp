#include "../../common/mmio_flip.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static unsigned checks;
static unsigned failures;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            ++failures;                                                                                                \
            std::fprintf(stderr, "mmio flip check %u line %d: %s\n", checks, __LINE__, #c);                            \
        }                                                                                                              \
    } while (0)

static VioGpuFlipTarget AcceptedTarget()
{
    VioGpuFlipTarget target = {};
    target.SourceId = 0;
    target.Segment = 1;
    target.ExpectedSegment = 1;
    target.Address = 0x2000;
    target.PlacementOffset = 0x2000;
    target.HasAllocation = true;
    target.OwnedByAdapter = true;
    target.ScanoutPrimary = true;
    target.PlacementValid = true;
    return target;
}

int main()
{
    // Public WDK bit positions; the WDK contract test asserts them against the
    // real bit-field unions.
    static_assert(VioGpuSourceAddressFlagModeChange == 1U && VioGpuSourceAddressFlagFlipImmediate == 2U && VioGpuSourceAddressFlagFlipOnNextVSync == 4U,
                  "DXGK_SETVIDPNSOURCEADDRESS_FLAGS");
    static_assert(VioGpuPresentFlagBlt == 1U && VioGpuPresentFlagColorFill == 2U && VioGpuPresentFlagFlip == 4U,
                  "DXGK_PRESENTFLAGS");
    static_assert(VioGpuFlipCapsOnVSyncMmIo == 2U, "DXGK_FLIPCAPS.FlipOnVSyncMmIo");
    static_assert(VioGpuDisplayMmioFlipCalls == 64U &&
                      VioGpuGuestAllocSubmitResult == VioGpuDisplayFlipPresentRejects + 1U &&
                      VioGpuTimingPathCalls == VioGpuMonitorLinkClaims + 1U &&
                      VioGpuOverlayCapsQueries == VioGpuTimingPathRejects + 1U &&
                      VioGpuOverlayCheckQueries == VioGpuOverlayCapsQueries + 1U &&
                      VioGpuOverlayCheckAccepts == VioGpuOverlayCheckQueries + 1U &&
                      VioGpuOverlayFlipCalls == VioGpuOverlayCheckAccepts + 1U &&
                      VioGpuOverlayFlipRejects == VioGpuOverlayFlipCalls + 1U &&
                      VioGpuOverlayFlipAccepts == VioGpuOverlayFlipRejects + 1U &&
                      VioGpuSynchronousLongestWaitSlices == VioGpuOverlayFlipAccepts + 1U &&
                      VioGpuFlipLatencyOver3ms == VioGpuSynchronousLongestWaitSlices + 1U &&
                      VioGpuFlipLatencyMaxUsec == VioGpuFlipLatencyOver3ms + 13U &&
                      VioGpuDisplayCounterCount == VioGpuFlipLatencyMaxUsec + 1U,
                  "new display counters follow the 64 historical slots");
    // Every slot is distinct and inside the published array.
    {
        unsigned slots[] = {VioGpuOverlayCheckQueries,        VioGpuOverlayCheckAccepts,
                            VioGpuOverlayFlipCalls,           VioGpuOverlayFlipRejects,
                            VioGpuOverlayFlipAccepts,
                            VioGpuDisplayMmioFlipCalls,       VioGpuDisplayMmioFlipRejects,
                            VioGpuDisplayMmioFlipRejectKind,  VioGpuDisplayMmioFlipLastFlags,
                            VioGpuDisplayMmioFlipApplied,     VioGpuDisplayMmioFlipApplyFailures,
                            VioGpuDisplayMmioFlipLastApplyStatus, VioGpuDisplayFlipPresentCalls,
                            VioGpuDisplayFlipPresentRejects,  VioGpuGuestAllocSubmitResult,
                            VioGpuGuestAllocSubmitted,        VioGpuGuestAllocCompleted,
                            VioGpuGuestAllocUnanswered,       VioGpuMonitorLinkQueries,
                            VioGpuMonitorLinkLastValue,       VioGpuMonitorLinkClaims,
                            VioGpuTimingPathCalls,            VioGpuTimingPathLastStatus,
                            VioGpuTimingPathWireFormat,       VioGpuTimingPathColorSpace,
                            VioGpuTimingPathRejects,          VioGpuOverlayCapsQueries,
                            VioGpuFlipLatencyOver3ms,         VioGpuFlipLatencyOver6ms,
                            VioGpuFlipPickupOver2ms,          VioGpuHostSurfaceReleaseWaitOver2ms,
                            VioGpuHostSurfaceWriterWaits,     VioGpuHostSurfaceIdleWaitOver1ms,
                            VioGpuHostSurfaceScanoutOver2ms,  VioGpuHostSurfaceAcceptOver2ms,
                            VioGpuFlipLastSlowPickupUsec,     VioGpuFlipLastSlowReleaseUsec,
                            VioGpuFlipLastSlowScanoutUsec,    VioGpuFlipLastSlowAcceptUsec,
                            VioGpuFlipLastSlowTotalUsec,      VioGpuFlipLatencyMaxUsec};
        for (unsigned i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i)
        {
            CHECK(slots[i] < VioGpuDisplayCounterCount);
            for (unsigned j = i + 1; j < sizeof(slots) / sizeof(slots[0]); ++j)
                CHECK(slots[i] != slots[j]);
        }
    }

    // The historical mode-change gate is unchanged: exactly ModeChange, no contexts.
    CHECK(VioGpuClassifySourceAddress(1U, 0U) == VioGpuSourceAddressModeChange);
    CHECK(VioGpuClassifySourceAddress(1U, 1U) == VioGpuSourceAddressInvalid);
    // dxgkrnl flips with ContextCount > 0; both flip timings are flips.
    for (unsigned contexts : {0U, 1U, 7U})
    {
        CHECK(VioGpuClassifySourceAddress(2U, contexts) == VioGpuSourceAddressFlip);
        CHECK(VioGpuClassifySourceAddress(4U, contexts) == VioGpuSourceAddressFlip);
    }
    // Every other flag word, including combinations and unadvertised stereo,
    // shared-primary, independent and move flips, stays invalid.
    unsigned accepted = 0;
    for (unsigned flags = 0; flags < 0x400U; ++flags)
    {
        const VioGpuSourceAddressKind kind = VioGpuClassifySourceAddress(flags, 1U);
        if (kind != VioGpuSourceAddressInvalid)
        {
            ++accepted;
            CHECK(flags == 2U || flags == 4U);
        }
    }
    CHECK(accepted == 2U);
    CHECK(VioGpuClassifySourceAddress(0x80000004U, 1U) == VioGpuSourceAddressInvalid);
    CHECK(VioGpuClassifySourceAddress(0U, 0U) == VioGpuSourceAddressInvalid);

    // Present: only a DMA-less flip without blit or fill is an MMIO flip.
    CHECK(VioGpuIsMmioFlipPresent(VioGpuPresentFlagFlip, true));
    CHECK(VioGpuIsMmioFlipPresent(VioGpuPresentFlagFlip | 0x8U /* FlipWithNoWait */, true));
    CHECK(VioGpuIsMmioFlipPresent(VioGpuPresentFlagFlip | 0x2000U /* RedirectedFlip */, true));
    CHECK(!VioGpuIsMmioFlipPresent(VioGpuPresentFlagFlip, false));
    CHECK(!VioGpuIsMmioFlipPresent(VioGpuPresentFlagBlt, true));
    CHECK(!VioGpuIsMmioFlipPresent(VioGpuPresentFlagBlt, false));
    CHECK(!VioGpuIsMmioFlipPresent(VioGpuPresentFlagFlip | VioGpuPresentFlagBlt, true));
    CHECK(!VioGpuIsMmioFlipPresent(VioGpuPresentFlagFlip | VioGpuPresentFlagColorFill, true));
    CHECK(!VioGpuIsMmioFlipPresent(0U, true));
    // The existing blit path keeps every present that carries a DMA buffer.
    for (unsigned flags = 0; flags < 0x4000U; ++flags)
    {
        CHECK(!VioGpuIsMmioFlipPresent(flags, false));
    }

    // Flip target validation, one refusal per missing property.
    CHECK(VioGpuValidateFlipTarget(AcceptedTarget()) == VioGpuFlipTargetAccepted);
    VioGpuFlipTarget host = AcceptedTarget();
    host.Segment = 2;
    host.ExpectedSegment = 2;
    CHECK(VioGpuValidateFlipTarget(host) == VioGpuFlipTargetAccepted);
    host.Segment = 1;
    CHECK(VioGpuValidateFlipTarget(host) == VioGpuFlipTargetBadSegment);
    host.Segment = 2;
    host.PlacementValid = false;
    CHECK(VioGpuValidateFlipTarget(host) == VioGpuFlipTargetNotPlaced);
    host.PlacementValid = true;
    host.ScanoutPrimary = false;
    CHECK(VioGpuValidateFlipTarget(host) == VioGpuFlipTargetNotPrimary);
    VioGpuFlipTarget t = AcceptedTarget();
    t.SourceId = 1;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetBadSource);
    t = AcceptedTarget();
    t.HasAllocation = false;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetNoAllocation);
    t = AcceptedTarget();
    t.OwnedByAdapter = false;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetForeign);
    t = AcceptedTarget();
    t.ScanoutPrimary = false;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetNotPrimary);
    t = AcceptedTarget();
    t.Segment = 2;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetBadSegment);
    t = AcceptedTarget();
    t.PlacementValid = false;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetNotPlaced);
    t = AcceptedTarget();
    t.Address = 0x3000;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetAddressMismatch);
    t = AcceptedTarget();
    t.Address = -1;
    t.PlacementOffset = ~0ULL;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetAddressMismatch);
    t = AcceptedTarget();
    t.Address = 0;
    t.PlacementOffset = 0;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetAccepted);
    // A ten-bit primary is scanned out through an MMIO flip only while Advanced
    // Color is usable; otherwise it is refused before segment/placement, but
    // never before ownership or type.
    t = AcceptedTarget();
    t.HighPrecision = true;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetHighPrecision);
    t.Segment = 2;
    t.PlacementValid = false;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetHighPrecision);
    t = AcceptedTarget();
    t.HighPrecision = true;
    t.ScanoutPrimary = false;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetNotPrimary);
    t.OwnedByAdapter = false;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetForeign);
    // Admitted Advanced Color makes exactly the ten-bit refusal go away, and
    // nothing else: every other gate still answers first.
    t = AcceptedTarget();
    t.HighPrecision = true;
    t.HighPrecisionAdmitted = true;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetAccepted);
    t.Segment = 2;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetBadSegment);
    t = AcceptedTarget();
    t.HighPrecision = true;
    t.HighPrecisionAdmitted = true;
    t.PlacementValid = false;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetNotPlaced);
    t = AcceptedTarget();
    t.HighPrecision = true;
    t.HighPrecisionAdmitted = true;
    t.Address = 0x3000;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetAddressMismatch);
    t = AcceptedTarget();
    t.HighPrecision = true;
    t.HighPrecisionAdmitted = true;
    t.ScanoutPrimary = false;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetNotPrimary);
    // Admission alone never relaxes an eight-bit flip's gates.
    t = AcceptedTarget();
    t.HighPrecisionAdmitted = true;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetAccepted);
    t.SourceId = 1;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetBadSource);
    // A foreign allocation is refused before its unrelated fields are trusted.
    t = AcceptedTarget();
    t.OwnedByAdapter = false;
    t.ScanoutPrimary = false;
    t.PlacementValid = false;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetForeign);

    std::printf("mmio flip: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
