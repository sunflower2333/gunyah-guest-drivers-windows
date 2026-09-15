#pragma once

/* CTA-861.3 HDR advertisement for the synthesized monitor.
 *
 * Windows decides a target's Advanced Color support from what the MONITOR
 * claims, not only from the driver's link capabilities: without an EDID
 * extension that declares BT.2020 colorimetry and the ST2084 EOTF, the panel
 * is an SDR panel and no colorimetry override or link capability changes that.
 * The synthesized fallback EDID is a bare 128-byte base block with no
 * extension, so this builds the missing one.
 *
 * Free of WDK headers so the byte layout is testable. The block is only ever
 * produced while the Host admits PQ output: an unadmitting Host must keep the
 * exact descriptor it has today.
 */

enum : unsigned
{
    VioGpuEdidBlockSize = 128,
    VioGpuEdidExtensionCountOffset = 126,
    VioGpuEdidChecksumOffset = 127,
    VioGpuHdrEdidSize = 2 * VioGpuEdidBlockSize,
};

/* CTA-861 data block headers: (tag << 5) | payload length, with the extended
 * tag as the first payload byte. */
enum : unsigned char
{
    VioGpuCtaExtensionTag = 0x02,
    VioGpuCtaRevision = 0x03,
    VioGpuCtaUseExtendedTag = 0x07,
    VioGpuCtaExtendedColorimetry = 0x05,
    VioGpuCtaExtendedHdrStaticMetadata = 0x06,
    /* Colorimetry byte 0, bit 7: BT.2020 RGB. */
    VioGpuCtaColorimetryBt2020Rgb = 0x80,
    /* Supported EOTFs: bit 0 traditional gamma SDR, bit 2 SMPTE ST2084. */
    VioGpuCtaEotfSdrAndSt2084 = 0x05,
    /* Static metadata descriptor type 1. */
    VioGpuCtaStaticMetadataType1 = 0x01,
};

inline unsigned char VioGpuEdidChecksum(const unsigned char *block)
{
    unsigned sum = 0;
    for (unsigned i = 0; i < VioGpuEdidBlockSize - 1; ++i)
    {
        sum += block[i];
    }
    return static_cast<unsigned char>((0x100U - (sum & 0xFFU)) & 0xFFU);
}

/* Writes base + one CTA-861 extension into out, and returns the byte count, or
 * 0 when the input cannot carry one. Refuses a base block that already
 * declares extensions: a Host-provided EDID owns its own extension chain and
 * must not be rewritten. */
inline unsigned VioGpuBuildHdrEdid(const unsigned char *base,
                                   unsigned baseSize,
                                   unsigned char *out,
                                   unsigned outSize)
{
    if (base == nullptr || out == nullptr || baseSize != VioGpuEdidBlockSize || outSize < VioGpuHdrEdidSize ||
        base[VioGpuEdidExtensionCountOffset] != 0)
    {
        return 0;
    }
    for (unsigned i = 0; i < VioGpuHdrEdidSize; ++i)
    {
        out[i] = 0;
    }
    for (unsigned i = 0; i < VioGpuEdidBlockSize; ++i)
    {
        out[i] = base[i];
    }
    /* The base block now carries an extension, so both its count and its
     * checksum change; a stale checksum makes the whole EDID invalid. */
    out[VioGpuEdidExtensionCountOffset] = 1;
    out[VioGpuEdidChecksumOffset] = VioGpuEdidChecksum(out);

    unsigned char *cta = out + VioGpuEdidBlockSize;
    cta[0] = VioGpuCtaExtensionTag;
    cta[1] = VioGpuCtaRevision;
    unsigned offset = 4;
    /* Colorimetry Data Block: BT.2020 RGB, no metadata profiles. */
    cta[offset++] = static_cast<unsigned char>((VioGpuCtaUseExtendedTag << 5) | 3U);
    cta[offset++] = VioGpuCtaExtendedColorimetry;
    cta[offset++] = VioGpuCtaColorimetryBt2020Rgb;
    cta[offset++] = 0;
    /* HDR Static Metadata Data Block: ST2084 plus the SDR EOTF, type 1
     * metadata. The optional desired-luminance bytes are omitted: the Host
     * reports the panel's real luminance through DVCL, and a guessed EDID
     * value would be a second, conflicting answer. */
    cta[offset++] = static_cast<unsigned char>((VioGpuCtaUseExtendedTag << 5) | 3U);
    cta[offset++] = VioGpuCtaExtendedHdrStaticMetadata;
    cta[offset++] = VioGpuCtaEotfSdrAndSt2084;
    cta[offset++] = VioGpuCtaStaticMetadataType1;
    /* No detailed timings follow, so the DTD offset is where the data block
     * collection ended and the rest is padding. */
    cta[2] = static_cast<unsigned char>(offset);
    cta[3] = 0;
    cta[VioGpuEdidChecksumOffset] = VioGpuEdidChecksum(cta);
    return VioGpuHdrEdidSize;
}
