// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <stdint.h>

// Access only under the scheduler interrupt's synchronization lock. The
// producer watermark is the contiguous retired prefix, never last-submitted.
// A notification carries the epoch in which it was prepared; reset supplies
// the completed floor of the new epoch without emitting DMA_COMPLETED.
struct VioGpuFencePublication
{
    uint32_t Epoch;
    uint32_t Fence;

    static bool After(uint32_t candidate, uint32_t previous)
    {
        return candidate != 0 && (previous == 0 || static_cast<int32_t>(candidate - previous) > 0);
    }

    bool Prepare(uint32_t notificationEpoch, uint32_t activeEpoch, uint32_t resetFloor,
                 uint32_t completed, bool preemption, uint32_t &reported)
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
