#include "../../shared/viogpu_display_color.h"
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>

int main()
{
    static_assert(offsetof(VIOGPU_SET_RESOURCE_COLOR, generation) == 40, "wire generation");
    static_assert(offsetof(VIOGPU_SET_RESOURCE_COLOR, resource_id) == 48, "wire resource");
    static_assert(offsetof(VIOGPU_SET_RESOURCE_COLOR, format) == 52, "wire format");
    static_assert(offsetof(VIOGPU_SET_RESOURCE_COLOR, min_mastering_luminance) == 100, "wire luminance");
    VIOGPU_SET_RESOURCE_COLOR request = {};
    request.generation = 7;
    request.resource_id = 42;
    request.format = VIOGPU_DISPLAY_FORMAT_AB30;
    request.encoding = VIOGPU_DISPLAY_COLOR_PQ;
    request.has_static_metadata = 1;
    const unsigned int xy[] = {35400, 14600, 8500, 39850, 6550, 2300, 15635, 16450};
    std::memcpy(request.chromaticities, xy, sizeof(xy));
    request.max_mastering_luminance = 1000;
    request.min_mastering_luminance = 50;
    request.max_content_light_level = 1000;
    request.max_frame_average_light_level = 400;
    assert(VioGpuValidDisplayMetadata(&request));
    assert(std::memcmp(reinterpret_cast<const unsigned char *>(&request) + 52, "AB30", 4) == 0);
    auto corrupt = request;
    corrupt.has_static_metadata = 0;
    assert(!VioGpuValidDisplayMetadata(&corrupt));
    corrupt = request;
    corrupt.chromaticities[0] = 50001;
    assert(!VioGpuValidDisplayMetadata(&corrupt));
    corrupt = request;
    corrupt.min_mastering_luminance = 10000001;
    assert(!VioGpuValidDisplayMetadata(&corrupt));
    corrupt.min_mastering_luminance = 10000000;
    assert(!VioGpuValidDisplayMetadata(&corrupt)); // min == max is not a mastering range
    corrupt = request;
    corrupt.chromaticities[0] = corrupt.chromaticities[1] = 0;
    assert(!VioGpuValidDisplayMetadata(&corrupt));
    corrupt = {};
    corrupt.encoding = VIOGPU_DISPLAY_COLOR_PQ;
    assert(VioGpuValidDisplayMetadata(&corrupt)); // explicit metadata clear
    corrupt = request;
    corrupt.max_frame_average_light_level = 1001;
    assert(!VioGpuValidDisplayMetadata(&corrupt));
    corrupt = request;
    corrupt.encoding = VIOGPU_DISPLAY_COLOR_HLG;
    assert(!VioGpuValidDisplayMetadata(&corrupt));
    auto transform = std::make_unique<VIOGPU_DISPLAY_TRANSFORM>();
    transform->version = 1;
    transform->size = sizeof(*transform);
    transform->kind = 2;
    transform->lut_count = 4096;
    transform->scalar = 0x3f800000U;
    assert(VioGpuValidDisplayTransform(transform.get()));
    transform->lut[4095][2] = 0x7f800000U;
    assert(!VioGpuValidDisplayTransform(transform.get()));
    transform->lut[4095][2] = 0;
    transform->matrix[4] = 0x7fc00000U;
    assert(!VioGpuValidDisplayTransform(transform.get()));
    transform->matrix[4] = 0;
    transform->kind = 1;
    transform->lut_count = 1025;
    assert(VioGpuValidDisplayTransform(transform.get()));
    transform->lut[1025][0] = 0x3f800000U;
    assert(!VioGpuValidDisplayTransform(transform.get()));
    static_assert(offsetof(VIOGPU_DISPLAY_TRANSFORM, lut) == 96, "shader/wire LUT offset");
    std::puts("DVCL wire, HDR10 metadata and float-bit transform validation: PASS");
}
