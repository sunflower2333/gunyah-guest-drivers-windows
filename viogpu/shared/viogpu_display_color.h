#pragma once
#include "viogpu_wddm_abi.h"

/* Project-private control queue extension, not an assigned virtio-gpu feature.
 * All fields are little endian. Supported guest architectures are little endian.
 * Discovery failure leaves HDR unavailable. Observations are never usable caps. */
#define VIOGPU_CMD_GET_DISPLAY_COLOR    0xd100U
#define VIOGPU_CMD_SET_RESOURCE_COLOR   0xd101U
#define VIOGPU_CMD_SET_TARGET_TRANSFORM 0xd102U
#define VIOGPU_RESP_DISPLAY_COLOR       0xd200U
#define VIOGPU_DISPLAY_COLOR_MAGIC      0x4c435644U
#define VIOGPU_DISPLAY_COLOR_VERSION    1U
#define VIOGPU_DISPLAY_COLOR_PQ         1U
#define VIOGPU_DISPLAY_COLOR_HLG        2U
#define VIOGPU_DISPLAY_FORMAT_AB30      0x30334241U
#define VIOGPU_DISPLAY_FORMAT_AR30      0x30335241U

typedef struct VIOGPU_DISPLAY_CONTROL_HEADER
{
    VIOGPU_WDDM_UINT32 type;
    VIOGPU_WDDM_UINT32 flags;
    VIOGPU_WDDM_UINT64 fence_id;
    VIOGPU_WDDM_UINT32 ctx_id;
    VIOGPU_WDDM_UINT32 ring_and_padding;
} VIOGPU_DISPLAY_CONTROL_HEADER;

typedef struct VIOGPU_GET_DISPLAY_COLOR
{
    VIOGPU_DISPLAY_CONTROL_HEADER hdr;
    VIOGPU_WDDM_UINT32 magic;
    VIOGPU_WDDM_UINT32 version;
    VIOGPU_WDDM_UINT32 size;
    VIOGPU_WDDM_UINT32 scanout_id;
} VIOGPU_GET_DISPLAY_COLOR;

typedef struct VIOGPU_DISPLAY_COLOR_RESPONSE
{
    VIOGPU_GET_DISPLAY_COLOR query;
    VIOGPU_WDDM_UINT64 generation;
    VIOGPU_WDDM_UINT32 observed_hdr_types;
    VIOGPU_WDDM_UINT32 usable_hdr_types;
    VIOGPU_WDDM_UINT32 max_luminance; /* 0.0001 cd/m2, UINT32_MAX means unknown */
    VIOGPU_WDDM_UINT32 max_average_luminance;
    VIOGPU_WDDM_UINT32 min_luminance;
    VIOGPU_WDDM_UINT32 reserved;
} VIOGPU_DISPLAY_COLOR_RESPONSE;

typedef struct VIOGPU_SET_RESOURCE_COLOR
{
    VIOGPU_GET_DISPLAY_COLOR query;
    VIOGPU_WDDM_UINT64 generation;
    VIOGPU_WDDM_UINT32 resource_id;
    VIOGPU_WDDM_UINT32 format;
    VIOGPU_WDDM_UINT32 encoding;
    VIOGPU_WDDM_UINT32 has_static_metadata;
    VIOGPU_WDDM_UINT32 chromaticities[8];             /* Rxy Gxy Bxy Wxy, units 0.00002 */
    VIOGPU_WDDM_UINT32 max_mastering_luminance;       /* cd/m2 */
    VIOGPU_WDDM_UINT32 min_mastering_luminance;       /* 0.0001 cd/m2 */
    VIOGPU_WDDM_UINT32 max_content_light_level;       /* cd/m2 */
    VIOGPU_WDDM_UINT32 max_frame_average_light_level; /* cd/m2 */
} VIOGPU_SET_RESOURCE_COLOR;

typedef struct VIOGPU_SET_TARGET_TRANSFORM
{
    VIOGPU_GET_DISPLAY_COLOR query;
    VIOGPU_WDDM_UINT64 generation;
    VIOGPU_WDDM_UINT32 reserved[2];
} VIOGPU_SET_TARGET_TRANSFORM;

/* IEEE754 bits, not kernel floating-point values. This fixed payload is an
 * owned data descriptor; never place its 49 KiB on the kernel stack. */
typedef struct VIOGPU_DISPLAY_TRANSFORM
{
    VIOGPU_WDDM_UINT32 version, size, kind, lut_count;
    VIOGPU_WDDM_UINT32 matrix[12], scalar, scale[3], offset[3], reserved;
    VIOGPU_WDDM_UINT32 lut[4096][3];
} VIOGPU_DISPLAY_TRANSFORM;

#if defined(__cplusplus)
inline bool VioGpuFiniteFloatBits(VIOGPU_WDDM_UINT32 bits)
{
    return (bits & 0x7f800000U) != 0x7f800000U;
}
inline bool VioGpuValidDisplayTransform(const VIOGPU_DISPLAY_TRANSFORM *t)
{
    if (t == nullptr || t->version != 1 || t->size != sizeof(*t) || t->reserved != 0 ||
        !((t->kind == 0 && t->lut_count == 0) || (t->kind == 1 && t->lut_count == 1025) ||
          (t->kind == 2 && t->lut_count == 4096)) ||
        !VioGpuFiniteFloatBits(t->scalar))
    {
        return false;
    }
    for (unsigned i = 0; i < 12; ++i)
    {
        if (!VioGpuFiniteFloatBits(t->matrix[i]))
        {
            return false;
        }
    }
    for (unsigned c = 0; c < 3; ++c)
    {
        if (!VioGpuFiniteFloatBits(t->scale[c]) || !VioGpuFiniteFloatBits(t->offset[c]))
        {
            return false;
        }
        for (unsigned i = 0; i < 4096; ++i)
        {
            if (!VioGpuFiniteFloatBits(t->lut[i][c]) || (i >= t->lut_count && (t->lut[i][c] & 0x7fffffffU) != 0))
            {
                return false;
            }
        }
    }
    return true;
}
inline bool VioGpuValidDisplayMetadata(const VIOGPU_SET_RESOURCE_COLOR *color)
{
    if (color == nullptr || color->has_static_metadata > 1 || color->max_mastering_luminance > 10000 ||
        color->min_mastering_luminance > 100000000 || color->max_content_light_level > 10000 ||
        color->max_frame_average_light_level > 10000)
    {
        return false;
    }
    for (unsigned int i = 0; i < 8; i += 2)
    {
        if (color->chromaticities[i] > 50000 || color->chromaticities[i + 1] > 50000 ||
            color->chromaticities[i] + color->chromaticities[i + 1] > 50000)
        {
            return false;
        }
        if (!color->has_static_metadata && (color->chromaticities[i] != 0 || color->chromaticities[i + 1] != 0))
        {
            return false;
        }
        if (color->has_static_metadata &&
            (color->chromaticities[i] + color->chromaticities[i + 1] == 0 ||
             (i == 6 && (color->chromaticities[i] == 0 || color->chromaticities[i + 1] == 0))))
        {
            return false;
        }
    }
    if (!color->has_static_metadata)
    {
        return color->max_mastering_luminance == 0 && color->min_mastering_luminance == 0 &&
               color->max_content_light_level == 0 && color->max_frame_average_light_level == 0;
    }
    return color->encoding == VIOGPU_DISPLAY_COLOR_PQ && color->max_mastering_luminance != 0 &&
           color->min_mastering_luminance < color->max_mastering_luminance * 10000 &&
           (color->max_content_light_level == 0 ||
            color->max_frame_average_light_level <= color->max_content_light_level);
}
static_assert(sizeof(VIOGPU_DISPLAY_CONTROL_HEADER) == 24, "DVCL control header");
static_assert(sizeof(VIOGPU_GET_DISPLAY_COLOR) == 40, "DVCL discovery request");
static_assert(sizeof(VIOGPU_DISPLAY_COLOR_RESPONSE) == 72, "DVCL discovery response");
static_assert(sizeof(VIOGPU_SET_RESOURCE_COLOR) == 112, "DVCL resource color");
static_assert(sizeof(VIOGPU_SET_TARGET_TRANSFORM) == 56, "DVCL target transform header");
static_assert(sizeof(VIOGPU_DISPLAY_TRANSFORM) == 49248, "DVCL float-bit transform payload");
#endif
