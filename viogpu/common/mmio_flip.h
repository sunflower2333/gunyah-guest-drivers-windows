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
    bool ScanoutPrimary;
    bool PlacementValid;
    /* Ten-bit primary. Set only by the Advanced Color build, where such
     * allocations exist. A flip carries no color space of its own, so one is
     * only scanned out when the adapter has a negotiated PQ mode to interpret
     * it with; the PASSIVE bind tags the resource before the Host sees it. */
    bool HighPrecision;
    /* The adapter's Advanced Color mode is usable right now: host PQ admitted,
     * monitor connected, no reset requested and the current fence epoch. Read
     * at DIRQL from interlocked state only. */
    bool HighPrecisionAdmitted;
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
    VioGpuFlipTargetHighPrecision = 8,
};

inline VioGpuFlipTargetStatus VioGpuValidateFlipTarget(const VioGpuFlipTarget &target)
{
    if (target.SourceId != 0)
    {
        return VioGpuFlipTargetBadSource;
    }
    if (!target.HasAllocation)
    {
        return VioGpuFlipTargetNoAllocation;
    }
    if (!target.OwnedByAdapter)
    {
        return VioGpuFlipTargetForeign;
    }
    if (!target.ScanoutPrimary)
    {
        return VioGpuFlipTargetNotPrimary;
    }
    /* An untagged ten-bit primary would be scanned out as if it were eight-bit
     * data; refuse it unless Advanced Color is usable for this very flip. */
    if (target.HighPrecision && !target.HighPrecisionAdmitted)
    {
        return VioGpuFlipTargetHighPrecision;
    }
    if (target.Segment != target.ExpectedSegment)
    {
        return VioGpuFlipTargetBadSegment;
    }
    if (!target.PlacementValid)
    {
        return VioGpuFlipTargetNotPlaced;
    }
    if (target.Address < 0 || static_cast<unsigned long long>(target.Address) != target.PlacementOffset)
    {
        return VioGpuFlipTargetAddressMismatch;
    }
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
    /* Why the native context was failed by a guest allocation. A control the
     * Host never answered (Unknown) and a control the Host refused (Rejected)
     * have the same visible consequence -- every later paging operation returns
     * STATUS_DEVICE_NOT_READY and the runtime drops the adapter -- but entirely
     * different causes, and telling them apart required disassembling the
     * recorded return address. These record it directly. */
    VioGpuGuestAllocSubmitResult = 73,
    VioGpuGuestAllocSubmitted = 74,
    VioGpuGuestAllocCompleted = 75,
    VioGpuGuestAllocUnanswered = 76,
    /* What we answered when dxgkrnl asked how the monitor link is wired. With
     * the EDID and the source mode both in place this is the last unmeasured
     * link in the Advanced Color chain: a zero claim and a never-asked query
     * look identical from outside. */
    VioGpuMonitorLinkQueries = 77,
    VioGpuMonitorLinkLastValue = 78,
    VioGpuMonitorLinkClaims = 79,
    /* SetTimingsFromVidPn is where dxgkrnl states the wire format and colour
     * space it actually chose, and it is the only DDI on the Advanced Color
     * enable path that reported nothing at all. It refuses anything that is
     * not exactly eight-bit sRGB or ten-bit PQ, and a refusal there leaves the
     * OS with no path to commit -- indistinguishable, from every other counter,
     * from dxgkrnl never having tried. Record the call, what it was asked for
     * and what it answered. */
    VioGpuTimingPathCalls = 80,
    VioGpuTimingPathLastStatus = 81,
    VioGpuTimingPathWireFormat = 82,
    VioGpuTimingPathColorSpace = 83,
    VioGpuTimingPathRejects = 84,
    /* Did dxgkrnl ever ask what overlay planes exist? Without an answer to that
     * there is no way to tell "MPO is unreachable" from "the probe never ran",
     * and the whole point of the caps probe is that distinction. */
    VioGpuOverlayCapsQueries = 85,
    /* MPO3. Check/flip are what make the overlay caps honest: declaring
     * MaxOverlays without them fails adapter start (CM_PROB_FAILED_POST_START).
     * Accepts are the ones that matter -- a non-zero flip accept means the
     * fullscreen app's own allocation reached the scanout without DWM copying
     * it, which is the whole point of the path. */
    VioGpuOverlayCheckQueries = 86,
    VioGpuOverlayCheckAccepts = 87,
    VioGpuOverlayFlipCalls = 88,
    VioGpuOverlayFlipRejects = 89,
    VioGpuOverlayFlipAccepts = 90,
    /* Most five second slices any synchronous control request has waited for
     * its host answer, sampled with each guest-allocation GEM_NEW. More than
     * one is a host stall that was waited out rather than turned into a
     * boot-long loss of the native transport. */
    VioGpuSynchronousLongestWaitSlices = 91,
    VioGpuDisplayCounterCount = 92,
};
