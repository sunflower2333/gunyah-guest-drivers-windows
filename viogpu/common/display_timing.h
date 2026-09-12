/* Internal virtual display timings; independent of the public escape ABI.
 * Detailed timings retain EDID pixel clock and blanking. Standard timings
 * specify only geometry/rate, so give those a consistent virtual raster. */
#pragma once

struct VIOGPU_DISPLAY_TIMING
{
    unsigned Width, Height, TotalWidth, TotalHeight, PixelClock;
};

static inline bool VioGpuTimingValid(const VIOGPU_DISPLAY_TIMING &t)
{
    return t.Width >= 640 && t.Height >= 480 && t.Width <= 8192 && t.Height <= 8192 &&
           t.TotalWidth > t.Width && t.TotalHeight > t.Height &&
           t.TotalWidth <= 16384 && t.TotalHeight <= 16384 &&
           t.PixelClock / (static_cast<unsigned long long>(t.TotalWidth) * t.TotalHeight) >= 20 &&
           t.PixelClock / (static_cast<unsigned long long>(t.TotalWidth) * t.TotalHeight) <= 360;
}

static inline VIOGPU_DISPLAY_TIMING VioGpuVirtualTiming(unsigned width, unsigned height, unsigned hz)
{
    VIOGPU_DISPLAY_TIMING t = {width, height, width + 160, height + 35, 0};
    const unsigned long long clock = static_cast<unsigned long long>(t.TotalWidth) * t.TotalHeight * hz;
    if (clock <= 0xffffffffULL)
        t.PixelClock = static_cast<unsigned>(clock);
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

static inline unsigned VioGpuReadEdidTimings(const unsigned char *edid, unsigned bytes,
                                          VIOGPU_DISPLAY_TIMING *modes, unsigned capacity)
{
    const unsigned char header[] = {0, 255, 255, 255, 255, 255, 255, 0};
    if (bytes < 128 || edid[18] != 1 || !VioGpuEdidBlockValid(edid))
        return 0;
    for (unsigned i = 0; i < 8; ++i)
        if (edid[i] != header[i])
            return 0;
    unsigned count = 0;
    // First valid DTD remains preferred, including a host-selected high rate.
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
    if (edid[126] && bytes >= 256 && edid[128] == 2 && VioGpuEdidBlockValid(edid + 128))
    {
        const unsigned start = edid[130];
        if (start >= 4 && start <= 109)
            for (unsigned offset = start; offset + 18 <= 127; offset += 18)
            {
                VIOGPU_DISPLAY_TIMING t = {};
                if (VioGpuDecodeDetailedTiming(edid + 128 + offset, t))
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
