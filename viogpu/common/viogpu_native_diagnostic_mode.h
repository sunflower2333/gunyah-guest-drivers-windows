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
