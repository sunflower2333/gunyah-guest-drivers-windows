#include "../../common/display_timing.h"
#include "host_edid_fixtures.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <initializer_list>
#include <fstream>
#include <vector>

static void displayidChecksum(unsigned char *block)
{
    const unsigned end = 5U + block[2];
    unsigned sum = 0;
    for (unsigned i = 1; i < end; ++i) sum += block[i];
    block[end] = static_cast<unsigned char>(0U - sum);
    sum = 0;
    for (unsigned i = 0; i < 127; ++i) sum += block[i];
    block[127] = static_cast<unsigned char>(0U - sum);
}

static void displayid(unsigned char *block, unsigned width, unsigned height,
                      unsigned hblank, unsigned vblank, unsigned long long pixelClock)
{
    std::memset(block, 0, 128);
    const unsigned char header[] = {0x70, 0x13, 23, 3, 0, 3, 0, 20};
    std::memcpy(block, header, sizeof(header));
    unsigned char *d = block + 8;
    const unsigned clock = static_cast<unsigned>(pixelClock / 10000 - 1);
    d[0] = static_cast<unsigned char>(clock);
    d[1] = static_cast<unsigned char>(clock >> 8);
    d[2] = static_cast<unsigned char>(clock >> 16);
    d[3] = 0x88;
    const unsigned values[] = {width, hblank, 64, 192, height, vblank, 1, 3};
    for (unsigned i = 0; i < 8; ++i) {
        const unsigned value = values[i] - 1;
        d[4 + i * 2] = static_cast<unsigned char>(value);
        d[5 + i * 2] = static_cast<unsigned char>(value >> 8);
    }
    d[9] |= 0x80; d[17] |= 0x80;
    displayidChecksum(block);
}

static void checksum(unsigned char *block)
{
    unsigned sum = 0;
    for (unsigned i = 0; i < 127; ++i) sum += block[i];
    block[127] = static_cast<unsigned char>(0U - sum);
}

static bool hostTimingMatches(const unsigned char *bytes, unsigned length, unsigned fixture)
{
    struct Expected {
        unsigned bytes, width, height, totalWidth, totalHeight, numerator, denominator;
        unsigned long long pixelClock, period;
    };
    static const Expected expected[] = {
        {256, 3040, 1904, 3600, 1954, 1450850, 8793, 1160680000ULL, 60606},
        {128, 1920, 1080, 2480, 1130, 578000, 3503, 462400000ULL, 60606},
        {256, 7680, 4320, 8240, 4370, 21605275, 90022, 8642110000ULL, 41667},
    };
    assert(fixture < 3);
    const auto &e = expected[fixture];
    VIOGPU_DISPLAY_TIMING modes[64] = {};
    unsigned rn = 0, rd = 0;
    if (length != e.bytes || VioGpuReadEdidTimings(bytes, length, modes, 64) == 0)
        return false;
    const auto &t = modes[0];
    return t.Width == e.width && t.Height == e.height && t.TotalWidth == e.totalWidth &&
           t.TotalHeight == e.totalHeight && t.PixelClock == e.pixelClock &&
           VioGpuTimingRational(t.PixelClock, static_cast<unsigned long long>(t.TotalWidth) * t.TotalHeight, rn, rd) &&
           rn == e.numerator && rd == e.denominator && VioGpuTimingPeriod100ns(t) == e.period;
}

int main(int argc, char **argv)
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
    unsigned char extended[384] = {};
    std::memcpy(extended, edid, 256);
    extended[126] = 2; checksum(extended);
    // CTA and DisplayID may coexist in either order (e.g. separate HDR CTA).
    extended[130] = 4; checksum(extended + 128);
    displayid(extended + 256, 3040, 1904, 560, 50, 1160680000);
    assert(VioGpuReadEdidTimings(extended, sizeof(extended), modes, 64) == 6);
    assert(modes[0].Width == 3040 && modes[0].Height == 1904);
    assert(modes[0].PixelClock == 1160680000 && modes[0].TotalWidth == 3600 && modes[0].TotalHeight == 1954);
    unsigned rn = 0, rd = 0;
    assert(VioGpuTimingRational(modes[0].PixelClock, 7034400, rn, rd));
    assert(rn == 1450850 && rd == 8793);
    assert(VioGpuTimingPeriod100ns(modes[0]) == 60606);
    unsigned char reversed[384];
    std::memcpy(reversed, extended, 128);
    std::memcpy(reversed + 128, extended + 256, 128);
    std::memcpy(reversed + 256, extended + 128, 128);
    assert(VioGpuReadEdidTimings(reversed, sizeof(reversed), modes, 64) == 6);
    assert(modes[0].Width == 3040 && modes[0].PixelClock == 1160680000);
    for (unsigned length = 256; length < sizeof(extended); ++length)
        assert(VioGpuReadEdidTimings(extended, length, modes, 64) == 5);
    unsigned char malformed[384];
    for (unsigned badLength : {1U, 2U, 22U, 24U, 122U, 255U}) {
        std::memcpy(malformed, extended, sizeof(extended));
        malformed[258] = static_cast<unsigned char>(badLength);
        checksum(malformed + 256);
        assert(VioGpuReadEdidTimings(malformed, sizeof(malformed), modes, 64) == 5);
    }
    std::memcpy(malformed, extended, sizeof(extended));
    malformed[284] ^= 1; checksum(malformed + 256); //outerchecksumOK,innerbad
    assert(VioGpuReadEdidTimings(malformed, sizeof(malformed), modes, 64) == 5);
    std::memcpy(malformed, extended, sizeof(extended));
    malformed[267] |= 0x10; displayidChecksum(malformed + 256); //interlace
    assert(VioGpuReadEdidTimings(malformed, sizeof(malformed), modes, 64) == 5);
    std::memcpy(malformed, extended, sizeof(extended));
    malformed[263] = 19; displayidChecksum(malformed + 256); //partialtypeI
    assert(VioGpuReadEdidTimings(malformed, sizeof(malformed), modes, 64) == 5);
    displayid(extended + 256, 7680, 4320, 560, 50, 8642110000ULL);
    assert(VioGpuReadEdidTimings(extended, sizeof(extended), modes, 64) == 6);
    assert(modes[0].PixelClock == 8642110000ULL);
    assert(VioGpuTimingPeriod100ns(modes[0]) == 41667);
    assert(!VioGpuTimingRational(0x100000001ULL, 1, rn, rd));
    assert(!VioGpuTimingRational(1, 0, rn, rd));
    assert(!VioGpuTimingValid({640, 480, 800, 515, 800ULL * 515 * 360 + 1}));
    assert(VioGpuReadEdidTimings(nullptr, 128, modes, 64) == 0);
    // These immutable fixtures are emitted by the actual Rust producer and run
    // under MSVC in WDK CI. Optional fresh binaries must match the same exact
    // matrix, including byte count; legacy/truncated71.83Hz cannot pass.
    assert(hostTimingMatches(host3040, host3040_len, 0));
    assert(hostTimingMatches(host1920, host1920_len, 1));
    assert(hostTimingMatches(host7680, host7680_len, 2));
    unsigned char oldHost[128];
    std::memcpy(oldHost, host3040, sizeof(oldHost));
    oldHost[54] = 0x62; oldHost[55] = 0xc5; // installed505.30MHz wrap
    oldHost[126] = 0; checksum(oldHost);
    assert(!hostTimingMatches(oldHost, sizeof(oldHost), 0));
    assert(VioGpuReadEdidTimings(oldHost, sizeof(oldHost), modes, 64) > 0);
    assert(modes[0].PixelClock == 505300000);
    assert(VioGpuTimingPeriod100ns(modes[0]) == 139212);
    assert(argc == 1 || argc == 4);
    for (int i = 1; i < argc; ++i) {
        std::ifstream file(argv[i], std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        assert(file && bytes.size() >= 128 && bytes.size() <= 1024);
        assert(hostTimingMatches(bytes.data(), static_cast<unsigned>(bytes.size()), static_cast<unsigned>(i - 1)));
        assert(VioGpuReadEdidTimings(bytes.data(), static_cast<unsigned>(bytes.size()), modes, 64) > 0);
        const auto &t = modes[0];
        assert(VioGpuTimingRational(t.PixelClock, static_cast<unsigned long long>(t.TotalWidth) * t.TotalHeight, rn, rd));
        std::printf("HOST_EDID %s active=%ux%u total=%ux%u pixel=%llu refresh=%u/%u period100ns=%llu\n",
                    argv[i], t.Width, t.Height, t.TotalWidth, t.TotalHeight, t.PixelClock, rn, rd, VioGpuTimingPeriod100ns(t));
    }
    puts("display timing: EDID/DisplayID/wide rational/capacity/malformed/CTA boundary PASS");
}
