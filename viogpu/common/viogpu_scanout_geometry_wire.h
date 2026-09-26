#pragma once

#include "../shared/viogpu_scanout_geometry.h"
#include "../shared/viogpu_scanout_profile.h"

/* Standalone layout, so tests need no WDK headers. This is exactly the
 * existing 24-byte virtio control header; all fields except Type are zero. */
#pragma pack(push, 4)
struct VIOGPU_GEOMETRY_CONTROL_HEADER
{
    unsigned int Type;
    unsigned int Flags;
    unsigned long long FenceId;
    unsigned int ContextId;
    unsigned char RingIndex;
    unsigned char Padding[3];
};
struct VIOGPU_QUERY_SCANOUT_GEOMETRY
{
    VIOGPU_GEOMETRY_CONTROL_HEADER Header;
    unsigned int ScanoutId;
    unsigned int Reserved;
};
struct VIOGPU_CONFIGURE_SCANOUT_GEOMETRY
{
    VIOGPU_GEOMETRY_CONTROL_HEADER Header;
    unsigned int ScanoutId;
    unsigned int Reserved;
    VIOGPU_SCANOUT_GEOMETRY Geometry;
};
struct VIOGPU_SCANOUT_GEOMETRY_RESPONSE
{
    VIOGPU_GEOMETRY_CONTROL_HEADER Header;
    unsigned int ScanoutId;
    unsigned int Flags;
    VIOGPU_SCANOUT_GEOMETRY Geometry;
};
struct VIOGPU_BIND_SCANOUT_GEOMETRY
{
    VIOGPU_GEOMETRY_CONTROL_HEADER Header;
    unsigned int ResourceId;
    unsigned int ScanoutId;
    unsigned long long HostResetGeneration;
    unsigned long long ModeGeneration;
};
struct VIOGPU_PRESENT_SCANOUT_GEOMETRY
{
    VIOGPU_GEOMETRY_CONTROL_HEADER Header;
    unsigned int ResourceId;
    unsigned int Reserved;
    unsigned long long Sequence;
    unsigned long long HostResetGeneration;
    unsigned long long ModeGeneration;
};
struct VIOGPU_QUERY_SCANOUT_PROFILE
{
    VIOGPU_GEOMETRY_CONTROL_HEADER Header;
    unsigned int ScanoutId;
    unsigned int Reserved;
};
struct VIOGPU_SCANOUT_PROFILE_RESPONSE
{
    VIOGPU_GEOMETRY_CONTROL_HEADER Header;
    unsigned int ScanoutId;
    unsigned int Reserved;
    VIOGPU_SCANOUT_PROFILE Profile;
    unsigned long long HostResetGeneration;
    unsigned long long ReservedTail;
};
struct VIOGPU_CONFIGURE_SCANOUT_PROFILE
{
    VIOGPU_GEOMETRY_CONTROL_HEADER Header;
    unsigned int ScanoutId;
    unsigned int Reserved;
    VIOGPU_SCANOUT_GEOMETRY Geometry;
    unsigned long long EndpointGeneration;
    unsigned long long ProfileGeneration;
};
#pragma pack(pop)

static_assert(sizeof(VIOGPU_GEOMETRY_CONTROL_HEADER) == 24, "geometry control header");
static_assert(sizeof(VIOGPU_QUERY_SCANOUT_GEOMETRY) == 32, "geometry query wire");
static_assert(sizeof(VIOGPU_CONFIGURE_SCANOUT_GEOMETRY) == 96, "geometry configure wire");
static_assert(sizeof(VIOGPU_SCANOUT_GEOMETRY_RESPONSE) == 96, "geometry response wire");
static_assert(sizeof(VIOGPU_BIND_SCANOUT_GEOMETRY) == 48, "geometry bind wire");
static_assert(sizeof(VIOGPU_PRESENT_SCANOUT_GEOMETRY) == 56, "geometry present/refresh wire");
static_assert(offsetof(VIOGPU_CONFIGURE_SCANOUT_GEOMETRY, Geometry) == 32, "geometry request offset");
static_assert(offsetof(VIOGPU_PRESENT_SCANOUT_GEOMETRY, HostResetGeneration) == 40, "present reset offset");
static_assert(sizeof(VIOGPU_QUERY_SCANOUT_PROFILE) == 32, "profile query wire");
static_assert(sizeof(VIOGPU_SCANOUT_PROFILE_RESPONSE) == 112, "profile response wire");
static_assert(sizeof(VIOGPU_CONFIGURE_SCANOUT_PROFILE) == 112, "profile configure wire");
static_assert(offsetof(VIOGPU_SCANOUT_PROFILE_RESPONSE, Profile) == 32, "profile response offset");
static_assert(offsetof(VIOGPU_SCANOUT_PROFILE_RESPONSE, HostResetGeneration) == 96, "profile host reset offset");
static_assert(offsetof(VIOGPU_CONFIGURE_SCANOUT_PROFILE, EndpointGeneration) == 96, "profile configure tokens offset");

static inline bool VioGpuScanoutGeometryResponseValid(const VIOGPU_SCANOUT_GEOMETRY_RESPONSE *response,
                                                     unsigned int responseSize)
{
    if (response == 0 || responseSize != sizeof(*response))
        return false;
    const auto &h = response->Header;
    return h.Type == 0xd220U && h.Flags == 0 && h.FenceId == 0 && h.ContextId == 0 &&
        h.RingIndex == 0 && h.Padding[0] == 0 && h.Padding[1] == 0 && h.Padding[2] == 0 &&
        response->ScanoutId == 0 && (response->Flags & ~1U) == 0 &&
        VioGpuScanoutGeometryValid(&response->Geometry, (response->Flags & 1U) != 0);
}

static inline bool VioGpuScanoutProfileResponseValid(const VIOGPU_SCANOUT_PROFILE_RESPONSE *response,
                                                    unsigned int responseSize)
{
    if (response == 0 || responseSize != sizeof(*response))
        return false;
    const auto &h = response->Header;
    return h.Type == 0xd222U && h.Flags == 0 && h.FenceId == 0 && h.ContextId == 0 &&
        h.RingIndex == 0 && h.Padding[0] == 0 && h.Padding[1] == 0 && h.Padding[2] == 0 &&
        response->ScanoutId == 0 && response->Reserved == 0 && response->ReservedTail == 0 &&
        response->HostResetGeneration != 0 && VioGpuScanoutProfileValid(&response->Profile);
}
