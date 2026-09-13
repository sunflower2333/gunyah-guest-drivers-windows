#pragma once

/* MMIO flip policy, free of WDK headers so it is testable. Bit values equal
 * DXGK_SETVIDPNSOURCEADDRESS_FLAGS and DXGK_PRESENTFLAGS; the WDK contract
 * test static_asserts them.
 *
 * Registered as WDDM 2.0, dxgkrnl refuses to create the render adapter unless
 * DXGK_DRIVERCAPS.FlipCaps.FlipOnVSyncMmIo is set ("Driver reports WDDM
 * version 2.0 or higher but does not support FlipOnVSyncMmIo cap", then
 * StartAdapter_AddAdapterFailed with STATUS_INVALID_PARAMETER). With that cap
 * a flip reaches DxgkDdiPresent without a DMA buffer, then
 * DxgkDdiSetVidPnSourceAddress at DIRQL with ContextCount > 0, and completes
 * when a CRTC vsync reports the same PrimaryAddress. */

constexpr unsigned VioGpuSourceAddressFlagModeChange = 0x1U;
constexpr unsigned VioGpuSourceAddressFlagFlipImmediate = 0x2U;
constexpr unsigned VioGpuSourceAddressFlagFlipOnNextVSync = 0x4U;

constexpr unsigned VioGpuPresentFlagBlt = 0x1U;
constexpr unsigned VioGpuPresentFlagColorFill = 0x2U;
constexpr unsigned VioGpuPresentFlagFlip = 0x4U;

/* DXGK_FLIPCAPS.Value with only FlipOnVSyncMmIo set. */
constexpr unsigned VioGpuFlipCapsOnVSyncMmIo = 0x2U;

enum VioGpuSourceAddressKind : unsigned
{
    VioGpuSourceAddressInvalid = 0,
    VioGpuSourceAddressModeChange = 1,
    VioGpuSourceAddressFlip = 2,
};

/* A mode change keeps its exact historical gate. A flip is exactly one of the
 * two flip timings; stereo, shared-primary transitions and independent flips
 * are never advertised, so any other flag word stays invalid. */
inline VioGpuSourceAddressKind VioGpuClassifySourceAddress(unsigned flags, unsigned contextCount)
{
    if (flags == VioGpuSourceAddressFlagModeChange)
    {
        return contextCount == 0 ? VioGpuSourceAddressModeChange : VioGpuSourceAddressInvalid;
    }
    if (flags == VioGpuSourceAddressFlagFlipImmediate || flags == VioGpuSourceAddressFlagFlipOnNextVSync)
    {
        return VioGpuSourceAddressFlip;
    }
    return VioGpuSourceAddressInvalid;
}

/* An MMIO flip present carries no DMA buffer and neither blit nor fill. */
inline bool VioGpuIsMmioFlipPresent(unsigned presentFlags, bool dmaBufferMissing)
{
    return dmaBufferMissing && (presentFlags & VioGpuPresentFlagFlip) != 0 &&
           (presentFlags & (VioGpuPresentFlagBlt | VioGpuPresentFlagColorFill)) == 0;
}

/* Everything the DIRQL half may read: nonpaged fields only, no locks. */
struct VioGpuFlipTarget
{
    unsigned SourceId;
    unsigned Segment;
    unsigned ExpectedSegment;
    long long Address;
    unsigned long long PlacementOffset;
    bool HasAllocation;
    bool OwnedByAdapter;
    bool StandardPrimary;
    bool PlacementValid;
};

/* Values are published as NativeDisplayMmioFlipRejectKind. */
enum VioGpuFlipTargetStatus : unsigned
{
    VioGpuFlipTargetAccepted = 0,
    VioGpuFlipTargetBadSource = 1,
    VioGpuFlipTargetNoAllocation = 2,
    VioGpuFlipTargetForeign = 3,
    VioGpuFlipTargetNotPrimary = 4,
    VioGpuFlipTargetBadSegment = 5,
    VioGpuFlipTargetNotPlaced = 6,
    VioGpuFlipTargetAddressMismatch = 7,
};

inline VioGpuFlipTargetStatus VioGpuValidateFlipTarget(const VioGpuFlipTarget &target)
{
    if (target.SourceId != 0)
        return VioGpuFlipTargetBadSource;
    if (!target.HasAllocation)
        return VioGpuFlipTargetNoAllocation;
    if (!target.OwnedByAdapter)
        return VioGpuFlipTargetForeign;
    if (!target.StandardPrimary)
        return VioGpuFlipTargetNotPrimary;
    if (target.Segment != target.ExpectedSegment)
        return VioGpuFlipTargetBadSegment;
    if (!target.PlacementValid)
        return VioGpuFlipTargetNotPlaced;
    if (target.Address < 0 || static_cast<unsigned long long>(target.Address) != target.PlacementOffset)
        return VioGpuFlipTargetAddressMismatch;
    return VioGpuFlipTargetAccepted;
}

/* Display counter slots appended after the 64 historical ones. */
enum : unsigned
{
    VioGpuDisplayMmioFlipCalls = 64,
    VioGpuDisplayMmioFlipRejects = 65,
    VioGpuDisplayMmioFlipRejectKind = 66,
    VioGpuDisplayMmioFlipLastFlags = 67,
    VioGpuDisplayMmioFlipApplied = 68,
    VioGpuDisplayMmioFlipApplyFailures = 69,
    VioGpuDisplayMmioFlipLastApplyStatus = 70,
    VioGpuDisplayFlipPresentCalls = 71,
    VioGpuDisplayFlipPresentRejects = 72,
    VioGpuDisplayCounterCount = 73,
};
