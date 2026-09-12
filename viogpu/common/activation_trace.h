#pragma once

/* Pointer-free registry ABI, shared by the production recorder and its tests.
 * Access is serialized by VioGpuDod's PASSIVE_LEVEL diagnostic mutex. */
enum VioGpuActivationPhase : unsigned int
{
    VioGpuActivationStarting = 1,
    VioGpuActivationActive = 2,
    VioGpuActivationStopping = 3,
    VioGpuActivationStopped = 4,
    VioGpuActivationFailed = 5,
    VioGpuActivationUnwinding = 6,
};

struct VioGpuActivationQuery
{
    unsigned int Sequence, Type, Status, InputSize, OutputSize, Phase, Lifecycle, ReadinessMask;
    unsigned int Values[8];
};

struct VioGpuActivationTrace
{
    unsigned int Magic, Version, Epoch, ByteSize;
    unsigned int DdiVersion, RenderOnly, Phase, StartStage, StartStatus, StartDetail;
    unsigned int TotalQueries, FailureCount, Count, CounterOverflow;
    unsigned int FirstStartFailureStage, FirstStartFailureStatus;
    VioGpuActivationQuery FirstFailure, LastFailure;
    VioGpuActivationQuery Entries[64];
};

static_assert(sizeof(unsigned int) == 4, "Activation trace DWORD width");
static_assert(sizeof(VioGpuActivationQuery) == 64, "Activation query ABI");
static_assert(sizeof(VioGpuActivationTrace) == 4288, "Activation trace ABI");

inline bool VioGpuActivationNextEpoch(unsigned int previous, unsigned int *epoch)
{
    if (epoch == nullptr || (previous & ~1U) > 0xfffffffcU)
        return false;
    *epoch = (previous & ~1U) + 2;
    return true;
}

inline void VioGpuActivationInitialize(VioGpuActivationTrace *trace, unsigned int epoch,
                                       unsigned int ddiVersion, unsigned int renderOnly)
{
    *trace = {};
    if (epoch == 0 || (epoch & 1) != 0 || renderOnly > 1)
        return;
    trace->Magic = 0x54434156; // VACT
    trace->Version = 2;
    trace->Epoch = epoch;
    trace->ByteSize = sizeof(*trace);
    trace->DdiVersion = ddiVersion;
    trace->RenderOnly = renderOnly;
    trace->Phase = VioGpuActivationStarting;
}

inline void VioGpuActivationStartStage(VioGpuActivationTrace *trace, unsigned int stage,
                                      unsigned int status, unsigned int detail)
{
    /* The existing transport also reports these stages during D0 recovery.
     * Preserve the original StartDevice result across reset/teardown. */
    if (trace->Version != 2 || trace->Phase != VioGpuActivationStarting)
        return;
    trace->StartStage = stage;
    trace->StartStatus = status;
    trace->StartDetail = detail;
    if ((status & 0x80000000U) != 0)
    {
        trace->FirstStartFailureStage = stage;
        trace->FirstStartFailureStatus = status;
        trace->Phase = VioGpuActivationFailed;
    }
}

inline bool VioGpuActivationAppend(VioGpuActivationTrace *trace, VioGpuActivationQuery entry)
{
    if (trace->Version != 2)
        return false;
    if (trace->TotalQueries == 0xffffffffU)
    {
        trace->CounterOverflow = 1;
        return true;
    }
    entry.Sequence = ++trace->TotalQueries;
    entry.Phase = trace->Phase;
    if (trace->Count < 64)
        trace->Entries[trace->Count++] = entry;
    if ((entry.Status & 0x80000000U) != 0)
    {
        if (trace->FailureCount++ == 0)
            trace->FirstFailure = entry;
        trace->LastFailure = entry;
    }
    return true;
}
