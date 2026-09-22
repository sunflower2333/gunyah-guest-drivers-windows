#pragma once

#include "../shared/viogpu_wddm_abi.h"
#include "viogpu_native_ahb_format.h"

/* Pure policy shared by the miniport and the host-side negative controls. */
static inline bool VioGpuNativeSurfaceRequestValid(const VIOGPU_WDDM_NATIVE_SURFACE *surface)
{
    if (surface == 0 || surface->Flags != 0 || surface->ContextId != 0 ||
        surface->Reserved[0] != 0 || surface->Reserved[1] != 0 || surface->Reserved[2] != 0)
        return false;
    if (surface->Opcode == VIOGPU_WDDM_ESCAPE_FREE_NATIVE_SURFACE)
        return surface->ShareKey != 0 && surface->ResetGeneration != 0 &&
               surface->ExpectedResetGeneration == surface->ResetGeneration;
    if (surface->Opcode != VIOGPU_WDDM_ESCAPE_ALLOCATE_NATIVE_SURFACE)
        return false;
    return surface->ExpectedResetGeneration == 0 && surface->ShareKey == 0 && surface->Size == 0 &&
           surface->ResetGeneration == 0 && surface->Modifier == 0 && surface->PlaneOffset == 0 &&
           surface->ResourceId == 0 && surface->Stride == 0 && surface->PlaneCount == 0 &&
           surface->LayoutFlags == 0 && surface->Width != 0 && surface->Height != 0 &&
           surface->Width <= 16384 && surface->Height <= 16384 &&
           (VIOGPU_WDDM_UINT64)surface->Width * surface->Height * 4 <= 256ULL * 1024 * 1024 &&
           (surface->Fourcc == VIOGPU_NATIVE_AHB_FOURCC_AR24 || surface->Fourcc == VIOGPU_NATIVE_AHB_FOURCC_AB24);
}

static inline bool VioGpuNativeSurfaceIdentityMatches(const VIOGPU_WDDM_NATIVE_SURFACE *a,
                                                       const VIOGPU_WDDM_NATIVE_SURFACE *b)
{
    return a->ShareKey == b->ShareKey && a->Size == b->Size && a->ResetGeneration == b->ResetGeneration &&
           a->Modifier == b->Modifier && a->PlaneOffset == b->PlaneOffset && a->ResourceId == b->ResourceId &&
           a->ContextId == b->ContextId && a->Width == b->Width && a->Height == b->Height &&
           a->Fourcc == b->Fourcc && a->Stride == b->Stride && a->PlaneCount == b->PlaneCount &&
           a->LayoutFlags == b->LayoutFlags;
}

static inline bool VioGpuNativeSurfaceMayRetire(bool ownerReleased, unsigned int imports, unsigned int allocations)
{
    return ownerReleased && imports == 0 && allocations == 0;
}
