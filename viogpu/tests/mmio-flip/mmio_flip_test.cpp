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
            std::fprintf(stderr, "mmio flip check %u line %d: %s\n", checks, __LINE__, #c);                          \
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
    target.StandardPrimary = true;
    target.PlacementValid = true;
    return target;
}

int main()
{
    // Public WDK bit positions; the WDK contract test asserts them against the
    // real bit-field unions.
    static_assert(VioGpuSourceAddressFlagModeChange == 1U && VioGpuSourceAddressFlagFlipImmediate == 2U &&
                      VioGpuSourceAddressFlagFlipOnNextVSync == 4U,
                  "DXGK_SETVIDPNSOURCEADDRESS_FLAGS");
    static_assert(VioGpuPresentFlagBlt == 1U && VioGpuPresentFlagColorFill == 2U && VioGpuPresentFlagFlip == 4U,
                  "DXGK_PRESENTFLAGS");
    static_assert(VioGpuFlipCapsOnVSyncMmIo == 2U, "DXGK_FLIPCAPS.FlipOnVSyncMmIo");
    static_assert(VioGpuDisplayMmioFlipCalls == 64U && VioGpuDisplayCounterCount == VioGpuDisplayFlipPresentRejects + 1U,
                  "new display counters follow the 64 historical slots");

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
    t.StandardPrimary = false;
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
    // A foreign allocation is refused before its unrelated fields are trusted.
    t = AcceptedTarget();
    t.OwnedByAdapter = false;
    t.StandardPrimary = false;
    t.PlacementValid = false;
    CHECK(VioGpuValidateFlipTarget(t) == VioGpuFlipTargetForeign);

    std::printf("mmio flip: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
