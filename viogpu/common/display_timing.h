/* Internal virtual display timings; independent of the public escape ABI.
 * Detailed timings retain EDID pixel clock and blanking. Standard timings
 * specify only geometry/rate, so give those a consistent virtual raster. */
#pragma once

struct VIOGPU_DISPLAY_TIMING
{
    unsigned Width, Height, TotalWidth, TotalHeight;
    unsigned long long PixelClock;
};

static inline unsigned long long VioGpuTimingGcd(unsigned long long a, unsigned long long b)
{
    while (b) { const unsigned long long next = a % b; a = b; b = next; }
    return a;
}

static inline bool VioGpuTimingRational(unsigned long long numerator, unsigned long long denominator,
                                       unsigned &n, unsigned &d)
{
    if (!numerator || !denominator)
        return false;
    const unsigned long long divisor = VioGpuTimingGcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
    if (numerator > 0xffffffffULL || denominator > 0xffffffffULL)
        return false;
    n = static_cast<unsigned>(numerator);
    d = static_cast<unsigned>(denominator);
    return true;
}

static inline bool VioGpuTimingValid(const VIOGPU_DISPLAY_TIMING &t)
{
    const unsigned long long total = static_cast<unsigned long long>(t.TotalWidth) * t.TotalHeight;
    unsigned numerator, denominator;
    return t.Width >= 640 && t.Height >= 480 && t.Width <= 8192 && t.Height <= 8192 &&
           t.TotalWidth > t.Width && t.TotalHeight > t.Height &&
           t.TotalWidth <= 16384 && t.TotalHeight <= 16384 &&
           t.PixelClock >= total * 20 && t.PixelClock <= total * 360 &&
           VioGpuTimingRational(t.PixelClock, total, numerator, denominator) &&
           VioGpuTimingRational(t.PixelClock, t.TotalWidth, numerator, denominator);
}

static inline VIOGPU_DISPLAY_TIMING VioGpuVirtualTiming(unsigned width, unsigned height, unsigned hz)
{
    VIOGPU_DISPLAY_TIMING t = {width, height, width + 160, height + 35, 0};
    const unsigned long long clock = static_cast<unsigned long long>(t.TotalWidth) * t.TotalHeight * hz;
    t.PixelClock = clock;
    return t;
}

static inline bool VioGpuDecodeDetailedTiming(const unsigned char *d, VIOGPU_DISPLAY_TIMING &t)
{
    // Interlaced/stereo timing needs a different scanout contract.
    if ((d[17] & 0xe1) || !(d[0] | d[1]))
        return false;
    t.Width = d[2] | ((d[4] & 0xf0U) << 4);
    t.Height = d[5] | ((d[7] & 0xf0U) << 4);
    t.TotalWidth = t.Width + d[3] + ((d[4] & 0x0fU) << 8);
    t.TotalHeight = t.Height + d[6] + ((d[7] & 0x0fU) << 8);
    t.PixelClock = (d[0] | (d[1] << 8)) * 10000U;
    return VioGpuTimingValid(t);
}

static inline bool VioGpuSameTiming(const VIOGPU_DISPLAY_TIMING &a, const VIOGPU_DISPLAY_TIMING &b)
{
    return a.Width == b.Width && a.Height == b.Height && a.TotalWidth == b.TotalWidth &&
           a.TotalHeight == b.TotalHeight && a.PixelClock == b.PixelClock;
}

static inline bool VioGpuAppendTiming(VIOGPU_DISPLAY_TIMING *modes, unsigned capacity, unsigned &count,
                                     const VIOGPU_DISPLAY_TIMING &timing)
{
    if (!VioGpuTimingValid(timing) || count >= capacity)
        return false;
    for (unsigned i = 0; i < count; ++i)
        if (VioGpuSameTiming(modes[i], timing))
            return false;
    modes[count++] = timing;
    return true;
}

static inline bool VioGpuEdidBlockValid(const unsigned char *edid)
{
    unsigned sum = 0;
    for (unsigned i = 0; i < 128; ++i)
        sum += edid[i];
    return (sum & 255) == 0;
}

static inline bool VioGpuDecodeDisplayIdTiming(const unsigned char *d, VIOGPU_DISPLAY_TIMING &t)
{
    // Type-I:24-bit10kHz units minusone,16-bit active/blank/sync sizes minusone.
    // The preferred flag is separate; reject interlace and stereo contracts.
    if (d[3] & 0x70U)
        return false;
    const unsigned clock = 1U + d[0] + (d[1] << 8) + (d[2] << 16);
    t.PixelClock = static_cast<unsigned long long>(clock) * 10000;
    t.Width = 1U + d[4] + (d[5] << 8);
    const unsigned hblank = 1U + d[6] + (d[7] << 8);
    const unsigned hfront = 1U + d[8] + ((d[9] & 0x7fU) << 8);
    const unsigned hsync = 1U + d[10] + (d[11] << 8);
    t.Height = 1U + d[12] + (d[13] << 8);
    const unsigned vblank = 1U + d[14] + (d[15] << 8);
    const unsigned vfront = 1U + d[16] + ((d[17] & 0x7fU) << 8);
    const unsigned vsync = 1U + d[18] + (d[19] << 8);
    t.TotalWidth = t.Width + hblank;
    t.TotalHeight = t.Height + vblank;
    return hfront + hsync <= hblank && vfront + vsync <= vblank && VioGpuTimingValid(t);
}

static inline bool VioGpuDisplayIdSectionValid(const unsigned char *extension)
{
    if (extension[0] != 0x70 || extension[1] != 0x13 || extension[2] > 121 ||
        !VioGpuEdidBlockValid(extension))
        return false;
    const unsigned end = 5U + extension[2];
    unsigned sum = 0;
    for (unsigned offset = 1; offset <= end; ++offset)
        sum += extension[offset];
    if (sum & 255)
        return false;
    // Validate the complete section before accepting any of its timings.
    for (unsigned offset = 5; offset < end;)
    {
        if (end - offset < 3 || extension[offset + 2] > end - offset - 3)
            return false;
        if (extension[offset] == 3 && (extension[offset + 1] != 0 || extension[offset + 2] % 20))
            return false;
        offset += 3U + extension[offset + 2];
    }
    return true;
}

static inline void VioGpuReadDisplayIdTimings(const unsigned char *edid, unsigned extensions,
                                            VIOGPU_DISPLAY_TIMING *modes, unsigned capacity,
                                            unsigned &count, bool preferred)
{
    for (unsigned index = 0; index < extensions; ++index)
    {
        const unsigned char *extension = edid + 128U * (index + 1);
        if (!VioGpuDisplayIdSectionValid(extension))
            continue;
        const unsigned end = 5U + extension[2];
        for (unsigned offset = 5; offset < end; offset += 3U + extension[offset + 2])
        {
            if (extension[offset] != 3)
                continue;
            for (unsigned pos = offset + 3; pos < offset + 3U + extension[offset + 2]; pos += 20)
            {
                VIOGPU_DISPLAY_TIMING timing = {};
                if (((extension[pos + 3] & 0x80U) != 0) == preferred &&
                    VioGpuDecodeDisplayIdTiming(extension + pos, timing))
                    VioGpuAppendTiming(modes, capacity, count, timing);
            }
        }
    }
}

static inline unsigned VioGpuReadEdidTimings(const unsigned char *edid, unsigned bytes,
                                          VIOGPU_DISPLAY_TIMING *modes, unsigned capacity)
{
    const unsigned char header[] = {0, 255, 255, 255, 255, 255, 255, 0};
    if (edid == 0 || modes == 0 || bytes < 128 || edid[18] != 1 || !VioGpuEdidBlockValid(edid))
        return 0;
    for (unsigned i = 0; i < 8; ++i)
        if (edid[i] != header[i])
            return 0;
    unsigned count = 0;
    const unsigned suppliedExtensions = bytes / 128 - 1;
    const unsigned extensions = edid[126] < suppliedExtensions ? edid[126] : suppliedExtensions;
    // DisplayID can encode the host's preferred timing above the655.35MHz
    // legacyDTD ceiling. Its explicit preferred flag outranks the base fallback.
    VioGpuReadDisplayIdTimings(edid, extensions, modes, capacity, count, true);
    // Otherwise first valid baseDTD remains preferred.
    for (unsigned offset = 54; offset + 18 <= 126; offset += 18)
    {
        VIOGPU_DISPLAY_TIMING t = {};
        if (VioGpuDecodeDetailedTiming(edid + offset, t))
            VioGpuAppendTiming(modes, capacity, count, t);
    }
    struct Established { unsigned byte, bit, width, height, hz; };
    static const Established established[] = {
        {35, 5, 640, 480, 60}, {35, 4, 640, 480, 67}, {35, 3, 640, 480, 72},
        {35, 2, 640, 480, 75}, {35, 1, 800, 600, 56}, {35, 0, 800, 600, 60},
        {36, 7, 800, 600, 72}, {36, 6, 800, 600, 75}, {36, 5, 832, 624, 75},
        {36, 3, 1024, 768, 60}, {36, 2, 1024, 768, 70}, {36, 1, 1024, 768, 75},
        {36, 0, 1280, 1024, 75}, {37, 7, 1152, 870, 75}
    };
    for (unsigned i = 0; i < sizeof(established) / sizeof(established[0]); ++i)
    {
        const Established &e = established[i];
        if (edid[e.byte] & (1U << e.bit))
            VioGpuAppendTiming(modes, capacity, count, VioGpuVirtualTiming(e.width, e.height, e.hz));
    }
    for (unsigned offset = 38; offset < 54; offset += 2)
    {
        if (edid[offset] <= 1)
            continue;
        const unsigned width = (edid[offset] + 31U) * 8U;
        unsigned height;
        switch (edid[offset + 1] >> 6)
        {
            case 0: height = edid[19] < 3 ? width : width * 10 / 16; break;
            case 1: height = width * 3 / 4; break;
            case 2: height = width * 4 / 5; break;
            default: height = width * 9 / 16; break;
        }
        VioGpuAppendTiming(modes, capacity, count,
                          VioGpuVirtualTiming(width, height, 60U + (edid[offset + 1] & 63U)));
    }
    // Parse CTA DTDs only within a supplied, checksummed extension. Data block
    // headers/audio/vendor bytes are not VICs and must never become modes.
    VioGpuReadDisplayIdTimings(edid, extensions, modes, capacity, count, false);
    for (unsigned index = 0; index < extensions; ++index)
    {
        const unsigned char *extension = edid + 128U * (index + 1);
        if (extension[0] != 2 || !VioGpuEdidBlockValid(extension))
            continue;
        const unsigned start = extension[2];
        if (start >= 4 && start <= 109)
            for (unsigned offset = start; offset + 18 <= 127; offset += 18)
            {
                VIOGPU_DISPLAY_TIMING t = {};
                if (VioGpuDecodeDetailedTiming(extension + offset, t))
                    VioGpuAppendTiming(modes, capacity, count, t);
            }
    }
    return count;
}

static inline unsigned long long VioGpuTimingPeriod100ns(const VIOGPU_DISPLAY_TIMING &t)
{
    if (!VioGpuTimingValid(t))
        return 0;
    return (10000000ULL * t.TotalWidth * t.TotalHeight + t.PixelClock / 2) / t.PixelClock;
}
