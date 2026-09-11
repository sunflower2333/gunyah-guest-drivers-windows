#pragma once
#include "viogpu_wddm_abi.h"

/* Project-private control queue extension, not an assigned virtio-gpu feature.
 * All fields are little endian. Supported guest architectures are little endian.
 * Discovery failure leaves HDR unavailable. Observations are never usable caps. */
#define VIOGPU_CMD_GET_DISPLAY_COLOR  0xd100U
#define VIOGPU_CMD_SET_RESOURCE_COLOR 0xd101U
#define VIOGPU_RESP_DISPLAY_COLOR     0xd200U
#define VIOGPU_DISPLAY_COLOR_MAGIC    0x4c435644U
#define VIOGPU_DISPLAY_COLOR_VERSION  1U
#define VIOGPU_DISPLAY_COLOR_PQ       1U
#define VIOGPU_DISPLAY_COLOR_HLG      2U
#define VIOGPU_DISPLAY_FORMAT_AB30    0x30334241U
#define VIOGPU_DISPLAY_FORMAT_AR30    0x30335241U

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

#if defined(__cplusplus)
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
    }
    if (!color->has_static_metadata)
    {
        return color->max_mastering_luminance == 0 && color->min_mastering_luminance == 0 &&
               color->max_content_light_level == 0 && color->max_frame_average_light_level == 0;
    }
    return color->encoding == VIOGPU_DISPLAY_COLOR_PQ && color->max_mastering_luminance != 0 &&
           color->min_mastering_luminance <= color->max_mastering_luminance * 10000 &&
           (color->max_content_light_level == 0 ||
            color->max_frame_average_light_level <= color->max_content_light_level);
}
static_assert(sizeof(VIOGPU_DISPLAY_CONTROL_HEADER) == 24, "DVCL control header");
static_assert(sizeof(VIOGPU_GET_DISPLAY_COLOR) == 40, "DVCL discovery request");
static_assert(sizeof(VIOGPU_DISPLAY_COLOR_RESPONSE) == 72, "DVCL discovery response");
static_assert(sizeof(VIOGPU_SET_RESOURCE_COLOR) == 112, "DVCL resource color");
#endif
