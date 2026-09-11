#pragma once

#include <stdint.h>

/* Packed 32-bit RGB, one plane, offset zero. Compute the last visible byte
 * in 64 bits; padding after the final row need not be part of the scanout. */
static inline bool VioGpuGuestScanoutBoundsValid(uint32_t width, uint32_t height, uint32_t stride, uint64_t backingSize)
{
    if (width == 0 || height == 0 || width > UINT32_MAX / 4 || stride < width * 4 || (stride & 3) != 0 ||
        backingSize == 0)
    {
        return false;
    }
    const uint64_t lastRow = (uint64_t)(height - 1) * stride;
    return lastRow < backingSize && (uint64_t)width * 4 <= backingSize - lastRow;
}
