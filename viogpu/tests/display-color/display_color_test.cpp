#undef NDEBUG
#include "../../shared/viogpu_display_color.h"
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
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
    // Advanced Color monitor connection policy.
    VIOGPU_DISPLAY_COLOR_RESPONSE none = {}, sdr = {}, hdr = {};
    sdr.generation = 3;
    sdr.observed_hdr_types = 3;
    hdr = sdr;
    hdr.usable_hdr_types = VIOGPU_DISPLAY_COLOR_PQ;
    hdr.max_luminance = 10000000;
    auto act = [](bool initialized,
                  bool discovered,
                  const VIOGPU_DISPLAY_COLOR_RESPONSE &n,
                  const VIOGPU_DISPLAY_COLOR_RESPONSE &c) {
        return VioGpuColorConnectionAction(initialized, true, discovered, &n, &c);
    };
    // An always-connected child reports its first connection and is never
    // pulsed, even when admission changes.
    assert(VioGpuColorConnectionAction(false, false, true, &none, &hdr) == VioGpuColorConnectionConnect);
    assert(VioGpuColorConnectionAction(true, false, true, &sdr, &hdr) == VioGpuColorConnectionNone);
    assert(VioGpuColorConnectionAction(true, false, false, &hdr, &none) == VioGpuColorConnectionNone);
    // First refresh always reports the monitor, even without DVCL.
    assert(act(false, false, none, none) == VioGpuColorConnectionConnect);
    assert(act(false, true, none, hdr) == VioGpuColorConnectionConnect);
    // SDR-only (usable zero, the installed host) never pulses the monitor:
    // not for an old host, a detached Surface, nor a new Surface generation.
    assert(act(true, false, none, none) == VioGpuColorConnectionNone);
    assert(act(true, false, sdr, none) == VioGpuColorConnectionNone);
    auto nextSdr = sdr;
    nextSdr.generation = 4;
    nextSdr.max_luminance = 5000000;
    assert(act(true, true, sdr, nextSdr) == VioGpuColorConnectionNone);
    auto observedOnly = sdr;
    observedOnly.usable_hdr_types = VIOGPU_DISPLAY_COLOR_HLG; // HLG alone is not PQ admission
    assert(act(true, true, sdr, observedOnly) == VioGpuColorConnectionNone);
    // Admission changes renegotiate in both directions.
    assert(act(true, true, sdr, hdr) == VioGpuColorConnectionReenumerate);
    assert(act(true, true, hdr, sdr) == VioGpuColorConnectionReenumerate);
    assert(act(true, false, hdr, none) == VioGpuColorConnectionReenumerate); // lost discovery withdraws HDR
    // While admitted, a new Surface generation or panel luminance renegotiates.
    assert(act(true, true, hdr, hdr) == VioGpuColorConnectionNone);
    auto nextHdr = hdr;
    nextHdr.generation = 4;
    assert(act(true, true, hdr, nextHdr) == VioGpuColorConnectionReenumerate);
    nextHdr = hdr;
    nextHdr.max_average_luminance = 4000000;
    assert(act(true, true, hdr, nextHdr) == VioGpuColorConnectionReenumerate);
    auto zeroGeneration = hdr;
    zeroGeneration.generation = 0; // never admitted without a generation
    assert(!VioGpuDisplayColorPqAdmitted(&zeroGeneration));
    assert(act(true, true, sdr, zeroGeneration) == VioGpuColorConnectionNone);
    // Monitor link capabilities: canonical FP16 scanout is not implemented, so
    // no Wide/HighColorSpace is ever claimed; with it, only admitted PQ may.
    assert(VioGpuMonitorLinkCapabilities(&hdr, false) == 0);
    assert(VioGpuMonitorLinkCapabilities(&sdr, false) == 0);
    assert(VioGpuMonitorLinkCapabilities(&sdr, true) == 0);
    assert(VioGpuMonitorLinkCapabilities(&zeroGeneration, true) == 0);
    assert(VioGpuMonitorLinkCapabilities(nullptr, true) == 0);
    assert(VioGpuMonitorLinkCapabilities(&hdr, true) ==
           (VIOGPU_LINK_CAP_WIDE_COLOR_SPACE | VIOGPU_LINK_CAP_HIGH_COLOR_SPACE));
    // The canonical scRGB scanout gate and the link claim are the same decision:
    // a build that does not implement the transform must never advertise it,
    // and an implementing build must still wait for admitted PQ output.
    for (const VIOGPU_DISPLAY_COLOR_RESPONSE *caps : {&hdr, &sdr, &zeroGeneration})
    {
        assert(!VioGpuScRgbScanoutAdmitted(caps, false));
        assert((VioGpuMonitorLinkCapabilities(caps, true) != 0) == VioGpuScRgbScanoutAdmitted(caps, true));
    }
    assert(VioGpuScRgbScanoutAdmitted(&hdr, true));
    assert(!VioGpuScRgbScanoutAdmitted(&sdr, true));
    assert(!VioGpuScRgbScanoutAdmitted(&zeroGeneration, true));
    assert(!VioGpuScRgbScanoutAdmitted(nullptr, true));
    // scRGB is an encoding, never a usable_hdr_types bit: the Host admits the
    // PQ output, so the two constants must not be confused.
    assert(VIOGPU_DISPLAY_ENCODING_SCRGB != VIOGPU_DISPLAY_COLOR_PQ &&
           VIOGPU_DISPLAY_ENCODING_SCRGB != VIOGPU_DISPLAY_COLOR_HLG);
    assert(VIOGPU_DISPLAY_FORMAT_AB4H == 0x48344241U); // 'A','B','4','H' little-endian
    {
        VIOGPU_DISPLAY_COLOR_RESPONSE hlgOnly = hdr;
        hlgOnly.usable_hdr_types = VIOGPU_DISPLAY_COLOR_HLG;
        assert(!VioGpuScRgbScanoutAdmitted(&hlgOnly, true)); // HLG admission is not PQ output
    }
    // Display detect control.
    bool hpd = true;
    assert(VioGpuDisplayDetectControl(&hpd, VioGpuDetectPollOne, 0, 0) == VioGpuDetectPoll);
    assert(VioGpuDisplayDetectControl(&hpd, VioGpuDetectPollOne, 1, 0) == VioGpuDetectInvalid);
    assert(VioGpuDisplayDetectControl(&hpd, VioGpuDetectPollAll, 5, 0) == VioGpuDetectPoll);
    assert(VioGpuDisplayDetectControl(&hpd, VioGpuDetectPollAll, 0, 1) == VioGpuDetectInvalid && hpd);
    assert(VioGpuDisplayDetectControl(&hpd, VioGpuDetectDisableHpd, 0, 0) == VioGpuDetectAccepted && !hpd);
    assert(VioGpuDisplayDetectControl(&hpd, VioGpuDetectPollOne, 0, 0) == VioGpuDetectInvalid);
    assert(VioGpuDisplayDetectControl(&hpd, VioGpuDetectPollAll, 0, 0) == VioGpuDetectInvalid);
    assert(VioGpuDisplayDetectControl(&hpd, VioGpuDetectEnableHpd, 0, 0) == VioGpuDetectAccepted && hpd);
    assert(VioGpuDisplayDetectControl(&hpd, VioGpuDetectUninitialized, 0, 0) == VioGpuDetectInvalid);
    assert(VioGpuDisplayDetectControl(&hpd, 5U, 0, 0) == VioGpuDetectInvalid);
    assert(VioGpuDisplayDetectControl(nullptr, VioGpuDetectEnableHpd, 0, 0) == VioGpuDetectInvalid);
    std::puts("DVCL wire, HDR10 metadata and float-bit transform validation: PASS");
    std::puts("Advanced Color monitor connection policy: PASS");
}
