#pragma once

/* Keep this helper independent of the user-mode CRT in kernel builds. */
static_assert(sizeof(unsigned int) == 4, "scanout dimensions are 32 bits");
static_assert(sizeof(unsigned long long) == 8, "scanout backing size is 64 bits");

/* Packed 32-bit RGB, one plane, offset zero. Compute the last visible byte
 * in 64 bits; padding after the final row need not be part of the scanout. */
static inline bool VioGpuGuestScanoutBoundsValid(unsigned int width,
                                                 unsigned int height,
                                                 unsigned int stride,
                                                 unsigned long long backingSize)
{
    if (width == 0 || height == 0 || width > 0xffffffffU / 4 || stride < width * 4 || (stride & 3) != 0 ||
        backingSize == 0)
    {
        return false;
    }
    const unsigned long long lastRow = (unsigned long long)(height - 1) * stride;
    return lastRow < backingSize && (unsigned long long)width * 4 <= backingSize - lastRow;
}
