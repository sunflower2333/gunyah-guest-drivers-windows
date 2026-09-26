#include "../../common/viogpu_scanout_geometry_wire.h"
#include "../../common/viogpu_native_mode_policy.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <initializer_list>

static VIOGPU_SCANOUT_GEOMETRY geometry(unsigned rotation, unsigned long long mode = 1)
{
    VIOGPU_SCANOUT_GEOMETRY g = {};
    g.Version = 1; g.Size = 64;
    g.LogicalWidth = 3040; g.LogicalHeight = 1904;
    g.StorageWidth = rotation & 1 ? 1904 : 3040;
    g.StorageHeight = rotation & 1 ? 3040 : 1904;
    g.ContentRotationCw = rotation;
    g.HostResetGeneration = 7; g.ModeGeneration = mode;
    return g;
}

int main()
{
    for (unsigned rotation = 0; rotation != 4; ++rotation)
    {
        const auto g = geometry(rotation);
        assert(VioGpuScanoutGeometryValid(&g));
        assert(VioGpuScanoutStorageMatches(&g, g.StorageWidth, g.StorageHeight));
        auto wrong = g; ++wrong.StorageWidth;
        assert(!VioGpuScanoutGeometryValid(&wrong));
        unsigned cw = 99;
        assert(VioGpuVidPnRotationToContentCw(rotation + 1, &cw));
        const unsigned expected[] = {0, 3, 2, 1};
        assert(cw == expected[rotation]);
    }
    const auto g = geometry(1);
    assert(!VioGpuScanoutStorageMatches(&g, 3040, 1904));
    for (unsigned field = 0; field != 12; ++field)
    {
        auto bad = g;
        switch (field) {
        case 0: bad.Version = 2; break;
        case 1: bad.Size = 63; break;
        case 2: bad.ContentRotationCw = 4; break;
        case 3: bad.Reserved = 1; break;
        case 4: bad.ReservedTail[0] = 1; break;
        case 5: bad.ReservedTail[1] = 1; break;
        case 6: bad.HostResetGeneration = 0; break;
        case 7: bad.ModeGeneration = 0; break;
        case 8: bad.StorageWidth = 0; break;
        case 9: bad.StorageHeight = 8193; break;
        case 10: bad.LogicalWidth = 8193; break;
        case 11: bad.LogicalHeight = 0; break;
        }
        assert(!VioGpuScanoutGeometryValid(&bad));
    }
    auto query = geometry(0, 0);
    assert(VioGpuScanoutGeometryValid(&query, false));
    assert(!VioGpuScanoutGeometryValid(&query));
    query = geometry(1, 0);
    assert(!VioGpuScanoutGeometryValid(&query, false));
    assert(!VioGpuScanoutGeometryValid(nullptr));
    unsigned cw = 99;
    for (unsigned rotation : {0U, 5U, 6U, 254U, 255U})
    {
        assert(!VioGpuVidPnRotationToContentCw(rotation, &cw));
        assert(cw == 99);
    }
    assert(!VioGpuVidPnRotationToContentCw(1, nullptr));
    assert(VioGpuNativeCommittedRotationSupported(1));
    for (unsigned rotation : {0U, 2U, 3U, 4U, 5U, 254U, 255U})
        assert(!VioGpuNativeCommittedRotationSupported(rotation));

    assert(VioGpuScanoutGeometryMayConfigure(nullptr, &g, 7));
    assert(!VioGpuScanoutGeometryMayConfigure(nullptr, &g, 8));
    assert(VioGpuScanoutGeometryMayConfigure(&g, &g, 7));
    auto changed = geometry(3);
    assert(!VioGpuScanoutGeometryMayConfigure(&g, &changed, 7));
    changed.ModeGeneration = 2;
    assert(VioGpuScanoutGeometryMayConfigure(&g, &changed, 7));
    assert(!VioGpuScanoutGeometryMayConfigure(&changed, &g, 7));
    auto identity = geometry(0, 3);
    assert(VioGpuScanoutGeometryMayConfigure(&changed, &identity, 7));
    auto reset = geometry(1, 1); reset.HostResetGeneration = 8;
    assert(VioGpuScanoutGeometryMayConfigure(&identity, &reset, 8));
    assert(!VioGpuScanoutGeometryMayConfigure(&reset, &identity, 7));
    auto maximum = g; maximum.ModeGeneration = ~0ULL;
    assert(!VioGpuScanoutGeometryMayConfigure(&maximum, &g, 7));

    VIOGPU_SCANOUT_BINDING bound = {42, 93, 112, 0, 0, g};
    assert(VioGpuScanoutBindingMatches(&bound, &g, 42, 93, 112, 1904, 3040));
    assert(!VioGpuScanoutBindingMatches(&bound, &changed, 42, 93, 112, 1904, 3040));
    assert(!VioGpuScanoutBindingMatches(&bound, &reset, 42, 93, 112, 1904, 3040));
    assert(!VioGpuScanoutBindingMatches(&bound, &g, 43, 93, 112, 1904, 3040));
    assert(!VioGpuScanoutBindingMatches(&bound, &g, 42, 94, 112, 1904, 3040));
    assert(!VioGpuScanoutBindingMatches(&bound, &g, 42, 93, 113, 1904, 3040));
    assert(!VioGpuScanoutBindingMatches(&bound, &g, 42, 93, 7, 1904, 3040));
    assert(!VioGpuScanoutBindingMatches(&bound, &g, 42, 93, 112, 3040, 1904));
    assert(!VioGpuScanoutBindingMatches(nullptr, &g, 42, 93, 112, 1904, 3040));
    assert(VioGpuScanoutGeometryEqual(&bound.Geometry, &g)); // No retry restamping.

    VIOGPU_SCANOUT_READINESS ready = {true, true, true, true, true, true};
    assert(VioGpuScanoutReady(ready));
    for (unsigned missing = 0; missing != 6; ++missing)
    {
        auto r = ready;
        switch (missing) {
        case 0: r.HostGeometry = false; break;
        case 1: r.ProducerFinalRender = false; break;
        case 2: r.RotationAwarePrimary = false; break;
        case 3: r.ModeCommitAndUpdate = false; break;
        case 4: r.ResourceBinding = false; break;
        case 5: r.AllPresentPaths = false; break;
        }
        assert(!VioGpuScanoutReady(r));
    }

    VIOGPU_CONFIGURE_SCANOUT_GEOMETRY wire = {};
    wire.Header.Type = 0xd21b; wire.Geometry = g;
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(&wire);
    assert(bytes[0] == 0x1b && bytes[1] == 0xd2 && bytes[32] == 1 && bytes[36] == 64);
    assert(bytes[40] == 0x70 && bytes[41] == 0x07); // 1904, actual storage
    assert(bytes[48] == 0xe0 && bytes[49] == 0x0b); // 3040, logical width
    assert(bytes[56] == 1 && bytes[64] == 7 && bytes[72] == 1);
    for (unsigned i = 4; i != 32; ++i) assert(bytes[i] == 0);
    VIOGPU_SCANOUT_GEOMETRY_RESPONSE response = {};
    response.Header.Type = 0xd220;
    response.Flags = 1; response.Geometry = g;
    assert(VioGpuScanoutGeometryResponseValid(&response, sizeof(response)));
    assert(!VioGpuScanoutGeometryResponseValid(&response, sizeof(response) - 1));
    assert(!VioGpuScanoutGeometryResponseValid(&response, sizeof(response) + 1));
    for (unsigned field = 0; field != 10; ++field)
    {
        auto bad = response;
        switch (field) {
        case 0: ++bad.Header.Type; break;
        case 1: bad.Header.Flags = 1; break;
        case 2: bad.Header.FenceId = 1; break;
        case 3: bad.Header.ContextId = 1; break;
        case 4: bad.Header.RingIndex = 1; break;
        case 5: bad.Header.Padding[0] = 1; break;
        case 6: bad.Header.Padding[1] = 1; break;
        case 7: bad.Header.Padding[2] = 1; break;
        case 8: bad.ScanoutId = 1; break;
        case 9: bad.Flags = 2; break;
        }
        assert(!VioGpuScanoutGeometryResponseValid(&bad, sizeof(bad)));
    }
    response.Flags = 0;
    assert(!VioGpuScanoutGeometryResponseValid(&response, sizeof(response)));
    response.Geometry = geometry(0, 0);
    assert(VioGpuScanoutGeometryResponseValid(&response, sizeof(response)));
    VIOGPU_SCANOUT_PROFILE profile = {1,64,7,1,1904,3040,3040,1904,23,29,{0,0}};
    assert(VioGpuScanoutProfileMatches(&profile, &g));
    for (unsigned field = 0; field != 16; ++field)
    {
        auto bad = profile;
        switch (field) {
        case 0: bad.Version = 2; break;
        case 1: bad.Size = 63; break;
        case 2: bad.Flags = 8; break;
        case 3: bad.Flags = 3; break;
        case 4: bad.ContentRotationCw = 4; break;
        case 5: bad.StorageWidth = 3040; break;
        case 6: bad.StorageHeight = 0; break;
        case 7: bad.LogicalWidth = 8193; break;
        case 8: bad.LogicalHeight = 0; break;
        case 9: bad.EndpointGeneration = 0; break;
        case 10: bad.ProfileGeneration = 0; break;
        case 11: bad.ReservedTail[0] = 1; break;
        case 12: bad.ReservedTail[1] = 1; break;
        case 13: bad.Flags = 0; break;
        case 14: bad.Flags = 1; break;
        case 15: bad.Flags = 6; break;
        }
        assert(!VioGpuScanoutProfileValid(&bad));
        assert(!VioGpuScanoutProfileMatches(&bad, &g));
    }
    for (unsigned rotation = 0; rotation != 4; ++rotation)
    {
        const auto shape = geometry(rotation);
        auto p = profile; p.ContentRotationCw = rotation;
        p.StorageWidth = shape.StorageWidth; p.StorageHeight = shape.StorageHeight;
        assert(VioGpuScanoutProfileMatches(&p, &shape));
    }
    VIOGPU_SCANOUT_PROFILE absent = {1,64,0,0,0,0,0,0,23,30,{0,0}};
    assert(VioGpuScanoutProfileValid(&absent) && !VioGpuScanoutProfileMatches(&absent, &g));
    assert(!VioGpuScanoutProfileValid(nullptr));
    VIOGPU_SCANOUT_PROFILE_RESPONSE profileResponse = {};
    profileResponse.Header.Type = 0xd222;
    profileResponse.Profile = profile; profileResponse.HostResetGeneration = 7;
    assert(VioGpuScanoutProfileResponseValid(&profileResponse, 112));
    assert(!VioGpuScanoutProfileResponseValid(&profileResponse, 111));
    assert(!VioGpuScanoutProfileResponseValid(&profileResponse, 113));
    for (unsigned field = 0; field != 12; ++field)
    {
        auto bad = profileResponse;
        switch (field) {
        case 0: ++bad.Header.Type; break;
        case 1: bad.Header.Flags = 1; break;
        case 2: bad.Header.FenceId = 1; break;
        case 3: bad.Header.ContextId = 1; break;
        case 4: bad.Header.RingIndex = 1; break;
        case 5: bad.Header.Padding[0] = 1; break;
        case 6: bad.Header.Padding[1] = 1; break;
        case 7: bad.Header.Padding[2] = 1; break;
        case 8: bad.ScanoutId = 1; break;
        case 9: bad.Reserved = 1; break;
        case 10: bad.HostResetGeneration = 0; break;
        case 11: bad.ReservedTail = 1; break;
        }
        assert(!VioGpuScanoutProfileResponseValid(&bad, sizeof(bad)));
    }
    bytes = reinterpret_cast<const unsigned char *>(&profileResponse);
    assert(bytes[0] == 0x22 && bytes[1] == 0xd2 && bytes[32] == 1 && bytes[36] == 64);
    assert(bytes[40] == 7 && bytes[44] == 1 && bytes[48] == 0x70 && bytes[49] == 0x07);
    assert(bytes[64] == 23 && bytes[72] == 29 && bytes[96] == 7 && bytes[104] == 0);
    puts("PASS geometry ABI, exact extents, rotation signs, readiness, immutable mode/reset/resource identities");
}
