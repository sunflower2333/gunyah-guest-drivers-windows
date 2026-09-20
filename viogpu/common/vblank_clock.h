#pragma once

// Interrupt-time units match ExSetTimer's relative 100 ns units. Keep the
// nominal phase independent of callback latency; no wall-clock adjustments.
struct VIOGPU_VBLANK_CLOCK
{
    unsigned long long Deadline100ns;
    unsigned long long Period100ns;
};

static inline bool VioGpuStartVblankClock(unsigned long long now,
                                        unsigned long long period,
                                        VIOGPU_VBLANK_CLOCK &clock)
{
    if (period == 0 || period > 0x7fffffffULL || now > ~0ULL - period)
    {
        return false;
    }
    clock.Deadline100ns = now + period;
    clock.Period100ns = period;
    return true;
}

static inline bool VioGpuNextVblankDeadline(unsigned long long now,
                                          VIOGPU_VBLANK_CLOCK &clock,
                                          unsigned long long &delay)
{
    const unsigned long long period = clock.Period100ns;
    if (period == 0 || period > 0x7fffffffULL || clock.Deadline100ns == 0)
    {
        return false;
    }
    // Advance directly to the next future grid point. A delayed DPC delivers
    // at most one vblank, never a burst for frames which were not displayed.
    delay = now < clock.Deadline100ns ? clock.Deadline100ns - now
                                     : period - (now - clock.Deadline100ns) % period;
    if (delay > period || now > ~0ULL - delay)
    {
        return false;
    }
    clock.Deadline100ns = now + delay;
    return true;
}

// Deadline remains on the same grid both before and after rearming. Derive
// raster phase from that grid rather than from the delayed callback arrival.
// This also works when a callback is several periods late, without inventing
// any vblank notifications for those missed periods.
static inline bool VioGpuVblankPosition(unsigned long long now,
                                       const VIOGPU_VBLANK_CLOCK &clock,
                                       unsigned long long &position)
{
    const unsigned long long period = clock.Period100ns;
    if (period == 0 || period > 0x7fffffffULL || clock.Deadline100ns < period)
    {
        return false;
    }
    if (now >= clock.Deadline100ns)
    {
        position = (now - clock.Deadline100ns) % period;
    }
    else
    {
        const unsigned long long remaining = clock.Deadline100ns - now;
        if (remaining > period)
        {
            return false;
        }
        position = (period - remaining) % period;
    }
    return true;
}
