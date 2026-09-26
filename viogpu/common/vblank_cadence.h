// SPDX-License-Identifier: BSD-3-Clause
#pragma once

enum VIOGPU_VBLANK_DELIVERY_OUTCOME
{
    VioGpuVblankDelivered,
    VioGpuVblankDisabled,
    VioGpuVblankHardwareGated,
    VioGpuVblankColorGated,
    VioGpuVblankNotifyFailed,
    VioGpuVblankOutcomeCount
};

// Protected by m_CrtcTimingLock. Never reset on mode/arm transitions.
struct VIOGPU_VBLANK_CADENCE_COUNTERS
{
    unsigned long long CallbackCount;
    unsigned long long DueCount;
    unsigned long long EarlyCount;
    unsigned long long InvalidPeriodCount;
    unsigned long long ResyncCount;
    unsigned long long MissedWholePeriods;
    unsigned long long ResyncPhaseTicks;
    unsigned long long MaxLatenessTicks;
    unsigned long long ArmCount;
    unsigned long long DisarmCount;
    unsigned long long ModeChanges;
    unsigned long long Delivery[VioGpuVblankOutcomeCount];
};

// One REG_BINARY NativeVblankCadenceSnapshot value, little endian, schema 1.
// QPC, geometry and counters come from the same timing-lock acquisition.
// DueCount may lead sum(Delivery) while a callback is between those stages.
// Delivered means successful Notify wrapper, not proof of physical scanout.
struct VIOGPU_VBLANK_CADENCE_SNAPSHOT
{
    unsigned int Version;
    unsigned int Size;
    unsigned long long AdapterStartQpc;
    unsigned long long SnapshotQpc;
    unsigned long long QpcFrequency;
    unsigned long long PeriodTicks;
    unsigned long long NextDueTicks;
    unsigned long long PixelClock;
    unsigned int Width;
    unsigned int Height;
    unsigned int TotalWidth;
    unsigned int TotalHeight;
    unsigned int Armed;
    unsigned int Enabled;
    VIOGPU_VBLANK_CADENCE_COUNTERS Counters;
};
static_assert(sizeof(VIOGPU_VBLANK_CADENCE_COUNTERS) == 128, "cadence counter ABI");
static_assert(sizeof(VIOGPU_VBLANK_CADENCE_SNAPSHOT) == 208, "cadence snapshot ABI");
