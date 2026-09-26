#pragma once

#include "../shared/viogpu_scanout_profile.h"

/* Internal mode ownership, not a wire layout. A reservation does not configure
 * the host or bind a resource. Candidate AHBs retain this complete value until
 * a successful mode commit makes it eligible for the first real BIND. */
struct VIOGPU_NATIVE_SCANOUT_MODE
{
    VIOGPU_GEOMETRY_U64 LocalResetGeneration;
    VIOGPU_SCANOUT_GEOMETRY Geometry;
    VIOGPU_SCANOUT_PROFILE Profile;
};

static inline bool VioGpuNativeScanoutModeValid(const VIOGPU_NATIVE_SCANOUT_MODE *mode)
{
    return mode != 0 && mode->LocalResetGeneration != 0 &&
        VioGpuScanoutProfileMatches(&mode->Profile, &mode->Geometry);
}

static inline bool VioGpuNativeScanoutModeEqual(const VIOGPU_NATIVE_SCANOUT_MODE *a,
                                                const VIOGPU_NATIVE_SCANOUT_MODE *b)
{
    return VioGpuNativeScanoutModeValid(a) && VioGpuNativeScanoutModeValid(b) &&
        a->LocalResetGeneration == b->LocalResetGeneration &&
        VioGpuScanoutGeometryEqual(&a->Geometry, &b->Geometry) &&
        VioGpuScanoutProfileEqual(&a->Profile, &b->Profile);
}
