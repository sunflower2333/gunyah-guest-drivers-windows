#pragma once
#include "../shared/viogpu_wddm_abi.h"

/* All transitions are protected by the miniport's access spin lock. Display
 * release observation is separate from GPU writer ownership. */
struct VIOGPU_NATIVE_AHB_ACCESS
{
    unsigned int Readers;
    bool Writer;
    bool PresentPending;
    bool WaitPending;
    bool Poisoned;
    VIOGPU_WDDM_UINT64 Sequence;
    VIOGPU_WDDM_UINT64 ReleasedSequence;
};

static inline bool VioGpuNativeAhbCanAccess(const VIOGPU_NATIVE_AHB_ACCESS *state, unsigned int access)
{
    return !state->Poisoned && !state->PresentPending && !state->Writer &&
           ((access & VIOGPU_WDDM_REFERENCE_WRITE) == 0 ||
            (state->Readers == 0 && state->Sequence == state->ReleasedSequence));
}

static inline bool VioGpuNativeImportTableValid(const VIOGPU_WDDM_RENDER_COMMAND *header,
                                               VIOGPU_WDDM_UINT64 ownedEnd,
                                               VIOGPU_WDDM_UINT64 packetSize)
{
    if (header->Flags == VIOGPU_WDDM_RENDER_FLAGS_NONE)
        return header->Reserved[0] == 0 && header->Reserved[1] == 0 && header->Reserved[2] == 0 &&
               header->Reserved[3] == 0 && header->CommandStreamOffset == ownedEnd;
    const VIOGPU_WDDM_UINT64 count = header->Reserved[1];
    const VIOGPU_WDDM_UINT64 end = ownedEnd + count * sizeof(VIOGPU_WDDM_IMPORTED_REFERENCE);
    return header->Flags == VIOGPU_WDDM_RENDER_IMPORTED_REFERENCES &&
           header->Reserved[0] == ownedEnd && count != 0 && count <= VIOGPU_WDDM_IMPORTED_REFERENCE_LIMIT &&
           header->Reserved[2] == VIOGPU_WDDM_IMPORTED_REFERENCES_VERSION && header->Reserved[3] == 0 &&
           end <= packetSize && header->CommandStreamOffset == end;
}
