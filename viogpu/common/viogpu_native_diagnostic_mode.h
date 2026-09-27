#pragma once

#include "viogpu_native_scanout_mode.h"
#include "display_timing.h"

/* REG_BINARY NativeScanoutDdiCapture: bounded array of240-byte records.
 * Flags bit0 source, bit1 path, bit2 signal. Stage1 raw Commit,2 final mode,
 * 3 raw shared-primary request. Raw DDI values are never orientation evidence. */
#pragma pack(push, 4)
struct VIOGPU_NATIVE_DIAGNOSTIC_DDI_RECORD
{
    unsigned Version, Stage, Status, Flags;
    VIOGPU_NATIVE_SCANOUT_MODE Mode;
    VIOGPU_GEOMETRY_U64 PrimaryHandle;
    unsigned SourceId, TargetId, Rotation, Scaling;
    unsigned SourceWidth, SourceHeight, VisibleWidth, VisibleHeight, Stride, Format;
    unsigned TargetWidth, TargetHeight, TotalWidth, TotalHeight;
    VIOGPU_GEOMETRY_U64 PixelClock;
    unsigned HSyncNumerator, HSyncDenominator, VSyncNumerator, VSyncDenominator;
};
#pragma pack(pop)
static_assert(sizeof(VIOGPU_NATIVE_DIAGNOSTIC_DDI_RECORD) == 240, "diagnostic DDI capture record");
static_assert(offsetof(VIOGPU_NATIVE_DIAGNOSTIC_DDI_RECORD, Mode) == 16, "DDI capture local reset offset");
static_assert(offsetof(VIOGPU_NATIVE_DIAGNOSTIC_DDI_RECORD, Mode) +
    offsetof(VIOGPU_NATIVE_SCANOUT_MODE, Geometry) == 24, "DDI capture geometry offset");
static_assert(offsetof(VIOGPU_NATIVE_DIAGNOSTIC_DDI_RECORD, Mode) +
    offsetof(VIOGPU_NATIVE_SCANOUT_MODE, Profile) == 88, "DDI capture profile offset");
static_assert(offsetof(VIOGPU_NATIVE_DIAGNOSTIC_DDI_RECORD, PrimaryHandle) == 152, "DDI capture primary offset");
static_assert(offsetof(VIOGPU_NATIVE_DIAGNOSTIC_DDI_RECORD, SourceId) == 160, "DDI capture source offset");
static_assert(offsetof(VIOGPU_NATIVE_DIAGNOSTIC_DDI_RECORD, SourceWidth) == 176, "DDI capture source geometry offset");
static_assert(offsetof(VIOGPU_NATIVE_DIAGNOSTIC_DDI_RECORD, TargetWidth) == 200, "DDI capture target offset");
static_assert(offsetof(VIOGPU_NATIVE_DIAGNOSTIC_DDI_RECORD, PixelClock) == 216, "DDI capture clock offset");

/* REG_BINARY NativeScanoutValidationCapture: last 32 completed calls, oldest
 * first. Separate from the first-16 Commit capture and its cursor reset epoch.
 * Sequence is adapter-local completion order; Time100ns is interrupt time.
 * Ddi: 1 IsSupported, 2 Enum. Step identifies the last callback attempted.
 * Result: IsSupported boolean; Enum 0 unchecked, 1 preparation failed,
 * 2 constraints rejected, 3 cofunctional. Data.Mode is the mode actually
 * prepared by Enum, never an additional query made for diagnostics.
 * Source/TargetModeStatus are valid only with Operations bits 0/1 set.
 * Data.Flags: source/path/signal bits retain the raw DDI capture meanings. */
#pragma pack(push, 4)
struct VIOGPU_NATIVE_VALIDATION_RECORD
{
    VIOGPU_NATIVE_DIAGNOSTIC_DDI_RECORD Data;
    VIOGPU_GEOMETRY_U64 Sequence, Time100ns;
    unsigned Ddi, Step, PivotType, PivotSourceId, PivotTargetId, Result, PathCount;
    unsigned SourceType, SourceColorBasis, SourceAccessMode, TargetScanLineOrdering;
    unsigned ScalingSupport, RotationSupport, Operations, SourceModeStatus, TargetModeStatus;
    VIOGPU_GEOMETRY_U64 RequiredSize, SegmentSize;
    unsigned ExpectedWidth, ExpectedHeight, ExpectedTotalWidth, ExpectedTotalHeight;
    VIOGPU_GEOMETRY_U64 ExpectedPixelClock;
    unsigned ExpectedHSyncNumerator, ExpectedHSyncDenominator;
    unsigned ExpectedVSyncNumerator, ExpectedVSyncDenominator, ExpectedScanLineOrdering;
};
#pragma pack(pop)
static_assert(sizeof(VIOGPU_NATIVE_VALIDATION_RECORD) == 380, "validation capture record");
static_assert(offsetof(VIOGPU_NATIVE_VALIDATION_RECORD, Ddi) == 256, "validation metadata offset");
static_assert(offsetof(VIOGPU_NATIVE_VALIDATION_RECORD, ExpectedWidth) == 336, "validation expected signal offset");

struct VIOGPU_NATIVE_VALIDATION_CAPTURE
{
    VIOGPU_GEOMETRY_U64 Sequence;
    unsigned Count;
    VIOGPU_NATIVE_VALIDATION_RECORD Records[32];
};

/* Caller serializes append and publication. Keep the newest calls even after
 * boot enumeration fills the capture; no reservation/Commit state is touched. */
static inline void VioGpuAppendNativeValidation(VIOGPU_NATIVE_VALIDATION_CAPTURE *capture,
    VIOGPU_NATIVE_VALIDATION_RECORD *record)
{
    if (capture->Count == 32)
    {
        for (unsigned i = 1; i < 32; ++i) capture->Records[i - 1] = capture->Records[i];
        --capture->Count;
    }
    record->Sequence = ++capture->Sequence;
    capture->Records[capture->Count++] = *record;
}

#pragma pack(push, 4)
struct VIOGPU_NATIVE_DIAGNOSTIC_CURSOR_RECORD
{
    VIOGPU_NATIVE_SCANOUT_MODE Mode;
    unsigned Stage, Flags, X, Y, Width, Height, Pitch, HotX, HotY, Visible;
};
#pragma pack(pop)
static_assert(sizeof(VIOGPU_NATIVE_DIAGNOSTIC_CURSOR_RECORD) == 176, "diagnostic cursor capture record");

/* One explicit diagnostic hypothesis: CCD ROTATE90 -> VidPN CCW90 ->
 * content CW3, physical primary/visible/mip/target, DXGI ModeDesc rotation2.
 * CCD and DXGI ordinals are not used as general rotation conversion rules. */
static inline bool VioGpuNativeDiagnosticModeTiming(const VIOGPU_NATIVE_SCANOUT_MODE *mode,
    const VIOGPU_DISPLAY_TIMING &logical, VIOGPU_DISPLAY_TIMING *physical)
{
    if (!VioGpuNativeScanoutModeValid(mode) || physical == 0 ||
        mode->Geometry.ContentRotationCw != 3 || !VioGpuTimingValid(logical) ||
        logical.Width != mode->Geometry.LogicalWidth || logical.Height != mode->Geometry.LogicalHeight)
        return false;
    const VIOGPU_DISPLAY_TIMING candidate = {logical.Height, logical.Width,
        logical.TotalHeight, logical.TotalWidth, logical.PixelClock};
    if (!VioGpuTimingValid(candidate) || candidate.Width != mode->Geometry.StorageWidth ||
        candidate.Height != mode->Geometry.StorageHeight) return false;
    *physical = candidate;
    return true;
}

static inline bool VioGpuNativeDiagnosticSourceMatches(const VIOGPU_NATIVE_SCANOUT_MODE *mode,
    unsigned width, unsigned height, unsigned visibleWidth, unsigned visibleHeight,
    unsigned stride, unsigned rotation, unsigned scaling)
{
    return VioGpuNativeScanoutModeValid(mode) && mode->Geometry.ContentRotationCw == 3 &&
        width == mode->Geometry.StorageWidth && height == mode->Geometry.StorageHeight &&
        visibleWidth == width && visibleHeight == height && stride == width * 4 &&
        rotation == 2 && scaling == 1;
}
