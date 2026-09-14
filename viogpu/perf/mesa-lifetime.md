# Paired Mesa lifetime experiment

The lifetime implementation was introduced in Mesa:

```
241131efe0daea2157e67a2f018feb5104f754fa
```

The current `external/mesa` gitlink and package receipt may select a later
revision that retains these changes; consult `a8xx-opengl.md` for that pairing.

This baseline added three UMD changes: opt-in final BO retirement, per-device
CPU submit scratch reuse, and aggregate lifetime/retry diagnostics. It does not
change the private ABI or bypass KMD native-range collision/host-detach checks.
The driver pipeline and VBUFFER implementation are unchanged by this pairing.

## Controlled activation

With `TU_WDDM_DEFERRED_BO_DESTROY` unset, the existing synchronous BO-destroy
policy is retained. `TU_WDDM_DEFERRED_BO_DESTROY=1` opts in before Vulkan-device
creation. Persistent CPU scratch reuse is independent and enabled in both cases.
Use a recoverable VM for the experimental mode; it has not passed target-device
acceptance. Do not globally enable it as part of installation.

Retired owners retain their KMT allocation, sparse token, rounded VMA and heap
charge until a fence-qualified KMT destruction succeeds. Busy and failed attempts
do not release ownership. Live plus retired objects share the existing 1024-slot
limit; pending work can therefore cause allocation pressure instead of eviction.
An OS destroy acknowledgment is not a host-detach acknowledgment: the existing
KMD range gate and guarded CreateAllocation busy retry remain essential.

CPU scratch contains the packet and Render reference arrays; it is not a GPU BO
reuse cache. The initial valid Render allocates it, later calls reuse it under
the WDDM mutex, and successful device teardown frees it. Only used prefixes are
cleared. Deferred retirement adds no background thread or per-submit drain.

`TU_WDDM_DIAGNOSTICS=1` enables aggregate queued/reaped/pending, fence-query,
busy/failure-reason and scratch allocation/reuse observations at successful
teardown. These counts do not measure GPU execution time or FPS. The existing
`analyze_submit_perf.py` parses KMD `viogpu perf:` records, not this new UMD format.

## Regression and target acceptance

The performance workflow includes `mesa-lifetime-pair`, checking out the exact
gitlink, checking its package receipt, executing the pinned Mesa production
fixtures under ASan/UBSan, and requiring four intentional regressions to fail
at their intended assertions. Its artifact records both driver and Mesa SHAs.
The standalone Mesa workflow also runs MSVC fixtures and separate ARM64 builds.
No GPU workload is executed by these source-level CI fixtures.

Before enabling by default, compare identical workloads with the switch off/on,
keeping the KMD, DXVK, VKD3D, shader cache and pipeline window constant. Exercise
allocation churn, mapped uploads, memory-budget pressure, buffer device address,
slow fences, repeated device creation/destruction, and reset/TDR. Reject stale
pixels/data, premature address reuse, unbounded retained owners or crashes.
Measure CPU time, frame-time distribution and throughput on the target.

The experimental teardown's preliminary wait uses 250 ms; subsequent KMT retries
have separate bounds, so this is not a total teardown deadline. Failure retains
the outer ownership graph for process lifetime rather than freeing live state.
Actual GPU BO reuse, target acceptance and measured performance remain separate.
