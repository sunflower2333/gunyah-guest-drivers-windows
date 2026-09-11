#include "../../common/viogpu_primary_scanout.h"

#include <assert.h>
#include <stdio.h>

int main()
{
    assert(VioGpuGuestScanoutBoundsValid(1920, 1080, 7680, 8294400));
    assert(!VioGpuGuestScanoutBoundsValid(1920, 1080, 7680, 8294399));
    const uint64_t paddedEnd = 1079ULL * 8192 + 7680;
    assert(VioGpuGuestScanoutBoundsValid(1920, 1080, 8192, paddedEnd));
    assert(!VioGpuGuestScanoutBoundsValid(1920, 1080, 8192, paddedEnd - 1));
    assert(VioGpuGuestScanoutBoundsValid(1, 1, 4096, 4));
    assert(!VioGpuGuestScanoutBoundsValid(1, 2, 4096, 4099));
    assert(VioGpuGuestScanoutBoundsValid(1, 2, 4096, 4100));
    assert(!VioGpuGuestScanoutBoundsValid(0, 1, 4, 4));
    assert(!VioGpuGuestScanoutBoundsValid(1, 0, 4, 4));
    assert(!VioGpuGuestScanoutBoundsValid(1, 1, 4, 0));
    assert(!VioGpuGuestScanoutBoundsValid(2, 1, 4, 8));
    assert(!VioGpuGuestScanoutBoundsValid(1, 1, 5, 8));
    assert(!VioGpuGuestScanoutBoundsValid(0x40000000, 1, UINT32_MAX, UINT64_MAX));
    const uint64_t largeEnd = (uint64_t)(UINT32_MAX - 1) * 0xfffffffcU + 4;
    assert(VioGpuGuestScanoutBoundsValid(1, UINT32_MAX, 0xfffffffcU, largeEnd));
    assert(!VioGpuGuestScanoutBoundsValid(1, UINT32_MAX, 0xfffffffcU, largeEnd - 1));
    puts("Guest scanout: 15 layout/bounds checks passed");
}
