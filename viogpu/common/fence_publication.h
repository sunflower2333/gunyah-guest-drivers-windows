// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Keep this policy freestanding: pulling MSVC's user-mode stdint.h into the
// WDK kernel CRT mixes runtime headers. UINT is 32 bits on every target here.
static_assert(sizeof(unsigned) == 4, "scheduler fences require 32-bit unsigned");

// Access only under the scheduler interrupt's synchronization lock. The
// producer watermark is the contiguous retired prefix, never last-submitted.
// A notification carries the epoch in which it was prepared; reset supplies
// the completed floor of the new epoch without emitting DMA_COMPLETED.
struct VioGpuFencePublication
{
    unsigned Epoch;
    unsigned Fence;

    static bool After(unsigned candidate, unsigned previous)
    {
        return candidate != 0 &&
               (previous == 0 || (candidate != previous && candidate - previous < 0x80000000U));
    }

    bool Prepare(unsigned notificationEpoch, unsigned activeEpoch, unsigned resetFloor,
                 unsigned completed, bool preemption, unsigned &reported)
    {
        if (notificationEpoch != activeEpoch)
            return false;
        if (Epoch != activeEpoch)
        {
            Epoch = activeEpoch;
            Fence = resetFloor;
        }
        const bool advanced = After(completed, Fence);
        if (advanced)
            Fence = completed;
        reported = Fence;
        // Preemption must still be acknowledged with the latest known fence,
        // including an idle engine's zero. Duplicate completion needs no IRQ.
        return preemption || advanced;
    }
};
