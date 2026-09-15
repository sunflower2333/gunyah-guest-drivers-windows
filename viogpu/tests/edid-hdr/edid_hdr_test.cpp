#include "../../common/edid_hdr.h"

#include <cstdio>
#include <cstring>

static unsigned checks;
static unsigned failures;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            ++failures;                                                                                                \
            std::fprintf(stderr, "edid hdr check %u line %d: %s\n", checks, __LINE__, #c);                            \
        }                                                                                                              \
    } while (0)

static void MakeBase(unsigned char *base)
{
    std::memset(base, 0, VioGpuEdidBlockSize);
    const unsigned char header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    std::memcpy(base, header, sizeof(header));
    base[8] = 0x49;
    base[9] = 0x14;
    base[VioGpuEdidExtensionCountOffset] = 0;
    base[VioGpuEdidChecksumOffset] = VioGpuEdidChecksum(base);
}

static unsigned BlockSum(const unsigned char *block)
{
    unsigned sum = 0;
    for (unsigned i = 0; i < VioGpuEdidBlockSize; ++i)
        sum += block[i];
    return sum & 0xFFU;
}

int main()
{
    unsigned char base[VioGpuEdidBlockSize];
    unsigned char out[VioGpuHdrEdidSize + 8];
    MakeBase(base);

    CHECK(VioGpuBuildHdrEdid(base, sizeof(base), out, VioGpuHdrEdidSize) == VioGpuHdrEdidSize);

    // The base block is preserved except for the extension count, and both
    // blocks must checksum to zero or the EDID is rejected wholesale.
    for (unsigned i = 0; i < VioGpuEdidExtensionCountOffset; ++i)
        CHECK(out[i] == base[i]);
    CHECK(out[VioGpuEdidExtensionCountOffset] == 1);
    CHECK(BlockSum(out) == 0);
    CHECK(BlockSum(out + VioGpuEdidBlockSize) == 0);
    // A stale base checksum is exactly the bug this must not have.
    CHECK(out[VioGpuEdidChecksumOffset] != base[VioGpuEdidChecksumOffset]);

    const unsigned char *cta = out + VioGpuEdidBlockSize;
    CHECK(cta[0] == 0x02 && cta[1] == 0x03);
    CHECK(cta[3] == 0); // no native DTDs, no audio/YCbCr claims

    // Colorimetry data block: extended tag 0x05, BT.2020 RGB only.
    CHECK(cta[4] == ((0x07 << 5) | 3));
    CHECK(cta[5] == 0x05);
    CHECK(cta[6] == 0x80);
    CHECK(cta[7] == 0x00);

    // HDR static metadata block: extended tag 0x06, ST2084 + SDR, type 1.
    CHECK(cta[8] == ((0x07 << 5) | 3));
    CHECK(cta[9] == 0x06);
    CHECK((cta[10] & 0x04) != 0); // ET_2 SMPTE ST2084
    CHECK((cta[10] & 0x01) != 0); // ET_0 traditional SDR still offered
    CHECK((cta[10] & 0x02) == 0); // traditional gamma HDR is not claimed
    CHECK(cta[11] == 0x01);

    // The DTD offset points past the data blocks; everything after is padding.
    CHECK(cta[2] == 12);
    for (unsigned i = 12; i < VioGpuEdidChecksumOffset; ++i)
        CHECK(cta[i] == 0);

    // Refusals: every one leaves the caller's buffer untouched.
    unsigned char guard[VioGpuHdrEdidSize];
    std::memset(guard, 0xAB, sizeof(guard));
    unsigned char scratch[VioGpuHdrEdidSize];
    std::memcpy(scratch, guard, sizeof(scratch));
    CHECK(VioGpuBuildHdrEdid(nullptr, VioGpuEdidBlockSize, scratch, sizeof(scratch)) == 0);
    CHECK(VioGpuBuildHdrEdid(base, VioGpuEdidBlockSize, nullptr, sizeof(scratch)) == 0);
    CHECK(VioGpuBuildHdrEdid(base, 127, scratch, sizeof(scratch)) == 0);
    CHECK(VioGpuBuildHdrEdid(base, 256, scratch, sizeof(scratch)) == 0);
    CHECK(VioGpuBuildHdrEdid(base, VioGpuEdidBlockSize, scratch, VioGpuHdrEdidSize - 1) == 0);
    CHECK(std::memcmp(scratch, guard, sizeof(scratch)) == 0);

    // A base that already declares extensions belongs to whoever built it.
    unsigned char hostEdid[VioGpuEdidBlockSize];
    MakeBase(hostEdid);
    hostEdid[VioGpuEdidExtensionCountOffset] = 1;
    hostEdid[VioGpuEdidChecksumOffset] = VioGpuEdidChecksum(hostEdid);
    CHECK(VioGpuBuildHdrEdid(hostEdid, sizeof(hostEdid), scratch, sizeof(scratch)) == 0);
    CHECK(std::memcmp(scratch, guard, sizeof(scratch)) == 0);

    // Checksum helper: a block that already sums to zero needs a zero byte.
    unsigned char zeroed[VioGpuEdidBlockSize];
    std::memset(zeroed, 0, sizeof(zeroed));
    CHECK(VioGpuEdidChecksum(zeroed) == 0);
    zeroed[0] = 1;
    CHECK(VioGpuEdidChecksum(zeroed) == 0xFF);

    std::printf("edid hdr: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
