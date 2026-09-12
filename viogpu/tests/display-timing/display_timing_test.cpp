#include "../../common/display_timing.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <initializer_list>

static void checksum(unsigned char *block)
{
    unsigned sum = 0;
    for (unsigned i = 0; i < 127; ++i) sum += block[i];
    block[127] = static_cast<unsigned char>(0U - sum);
}

int main()
{
    unsigned char edid[256] = {0, 255, 255, 255, 255, 255, 255, 0};
    edid[18] = 1; edid[19] = 4;
    // Generic DTD:1280x1024,blanking280x45,165Hz ->27516*10kHz.
    // Pixel-clock quantization yields164.99964Hz, never an integer60Hz.
    const unsigned char dtd[] = {0x7c, 0x6b, 0, 0x18, 0x51, 0, 0x2d, 0x40,
                                 0x30, 0x20, 0x35, 0, 0, 0, 0, 0, 0, 0x1e};
    std::memcpy(edid + 54, dtd, 18);
    edid[38] = 129; edid[39] = 0x80; //1280x1024@60, same geometry, distinct rate
    edid[40] = 169; edid[41] = 0xc0; //1600x900@60 (aspect ratio in TOP bits)
    edid[42] = 129; edid[43] = 0xa5; //1280x1024@97
    checksum(edid);
    VIOGPU_DISPLAY_TIMING modes[64] = {};
    unsigned count = VioGpuReadEdidTimings(edid, sizeof(edid), modes, 64);
    assert(count == 4);
    assert(modes[0].Width == 1280 && modes[0].Height == 1024);
    assert(modes[0].TotalWidth == 1560 && modes[0].TotalHeight == 1069);
    assert(modes[0].PixelClock == 275160000);
    assert(VioGpuTimingPeriod100ns(modes[0]) == 60606);
    assert(VioGpuTimingPeriod100ns(modes[1]) == 166667);
    assert(modes[2].Width == 1600 && modes[2].Height == 900);
    assert(VioGpuTimingPeriod100ns(modes[3]) == 103093);
    // Current crosvm1920x1080@165:blanking560x50; clock rounded to100kHz
    // by edid.rs. Preserve the resulting165.001427Hz instead of truncating it.
    const unsigned char hostDtd[] = {0xa0, 0xb4, 0x80, 0x30, 0x72, 0x38, 0x32, 0x40,
                                    64, 192, 0x13, 0, 0, 0, 0, 0, 0, 0x1e};
    VIOGPU_DISPLAY_TIMING host = {};
    assert(VioGpuDecodeDetailedTiming(hostDtd, host));
    assert(host.PixelClock == 462400000 && host.TotalWidth == 2480 && host.TotalHeight == 1130);
    assert(host.Width == 1920 && host.Height == 1080 && VioGpuTimingPeriod100ns(host) == 60606);
    assert(!VioGpuAppendTiming(modes, 64, count, modes[0]) && count == 4);
    //Capacity, truncation and malformed descriptors must never access outside buffers.
    VIOGPU_DISPLAY_TIMING bounded[2] = {};
    bounded[1].PixelClock = 0xdeadbeef;
    assert(VioGpuReadEdidTimings(edid, sizeof(edid), bounded, 1) == 1);
    assert(bounded[1].PixelClock == 0xdeadbeef);
    assert(VioGpuReadEdidTimings(edid, 127, modes, 64) == 0);
    edid[127] ^= 1;
    assert(VioGpuReadEdidTimings(edid, sizeof(edid), modes, 64) == 0);
    checksum(edid);
    edid[71] |= 0x80; checksum(edid); //unsupported interlace
    assert(VioGpuReadEdidTimings(edid, sizeof(edid), modes, 64) == 3);
    edid[71] &= 0x7f; checksum(edid);
    edid[126] = 1; checksum(edid);
    edid[128] = 2; edid[129] = 3; edid[130] = 4;
    std::memcpy(edid + 132, dtd, 18);
    edid[132] = 0x02; edid[133] = 0x3a; //148.50MHz with same raster
    checksum(edid + 128);
    assert(VioGpuReadEdidTimings(edid, sizeof(edid), modes, 64) == 5);
    assert(VioGpuReadEdidTimings(edid, 128, modes, 64) == 4);
    edid[130] = 127; checksum(edid + 128);
    assert(VioGpuReadEdidTimings(edid, sizeof(edid), modes, 64) == 4);
    for (unsigned rate : {60U, 75U, 120U, 144U, 165U, 240U})
    {
        const auto t = VioGpuVirtualTiming(1920, 1080, rate);
        assert(VioGpuTimingValid(t));
        const auto period = VioGpuTimingPeriod100ns(t);
        assert(period * rate >= 10000000U - rate && period * rate <= 10000000U + rate);
    }
    puts("display timing: EDID/rational refresh/capacity/malformed/CTA PASS");
}
