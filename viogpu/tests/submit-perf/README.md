# Bounded native-submit regression

This change materializes the earlier binding-snapshot and native-payload optimizations in C++,
fixes the legacy exact-replacement migration, adds aggregate statistics, and pipelines Render
without weakening host/VBUFFER ownership. It does not change a guest/host or UMD wire ABI.

## Ownership and ordering

`Queued -> WorkerOwned -> HostPending -> Idle` is the normal asynchronous Render path.
Only Render is pipeline-eligible. A Present or paging item remains a FIFO drain barrier:
it cannot start until prior Render host owners retire, and later Render cannot overtake it.
The admission limit includes requests held in the lower virtqueue software backlog.

The dispatch worker takes a separate temporary submission reference before calling the
host enqueue function. A terminal callback may execute before that function returns.
The dispatch handoff neither releases the durable work reference nor retires allocations.
Completion, failure and cancellation keep the existing terminal-reference machinery.
Reset/close is not idle until both the executing worker and every host-owned item drain.

The buffer pool uses an internal owner pointer and live-list flag, under its existing
spin lock, to unlink in constant time. Reset/close revoke that flag before detaching lists;
a terminal-claim timeout restores it on reinsert. The API still requires a valid buffer
pointer: this is not a reclamation scheme for arbitrary stale pointers.

## Build and rollback

The default `VioGpuNativePipelineWindow` MSBuild property is 64. Set
`/p:VioGpuNativePipelineWindow=1` for a serialized control, or use 32/64/128 for A/B tests.
The compile-time bound rejects values outside 1..128. Window 1 preserves serialization
while retaining the safety and lookup fixes. No build step rewrites source files.

## Aggregate diagnostics

Statistics are cumulative over the adapter lifetime and emitted only when an existing
passive-queue drain succeeds and new work has been accepted since the previous report.
They are not printed per submit. The three-line `viogpu perf:` record contains:

- accepted/dispatched/retired work and queued cancellations; Render bytes/reference sums
  and maximum references; peak pending FIFO and peak host-pending ownership.
- Render retirement count, total/maximum dispatch-to-terminal duration in 100 ns units,
  and successful pipeline handoffs.

`host_pending_peak` includes software backlog; it is **not** a pure physical-host inflight
metric. `retire_100ns` includes binding checks and queue time; it is **not** GPU execution
or fence-only latency. Average references is refs/render; average retirement duration is
total/render_retired. Avoid comparing cumulative snapshots without taking deltas.
No live IOCTL or UMD ABI was added solely to expose diagnostics.

## Running the fixtures

From the repository root:

```sh
python viogpu/tests/submit-perf/run.py
python viogpu/tests/submit-perf/run.py --negative-control-serial
python viogpu/tests/submit-perf/run.py --negative-control-worker-reference
python viogpu/tests/submit-perf/run.py --negative-control-pool-reset
python -m unittest discover -s viogpu/package -p 'test_*.py'
python viogpu/viogpuwddm/check-contract.py
```

The runner extracts actual production methods and record declarations. It compiles them
at window sizes 1, 32, 64 and 128 against kernel/host mocks (GCC with ASan/UBSan, or MSVC
for positive fixtures). Negative controls are run in Linux sanitizer CI and must fail for
the expected assertion or heap-use-after-free, not merely return a generic failure.
The pool test frees 2048 buffers in shuffled order and checks reuse, foreign-pool rejection,
still-cached duplicate release, detached reset callbacks, timeout reinsert and payload cleanup.
The scheduler test exercises bounds, out-of-order retirement, a drain barrier, cancellation,
512 inline completions, enqueue failure, reset, and 30 deterministic interleaved schedules.
These are deterministic interleavings, not a proof of all simultaneous kernel races.

## Hardware acceptance still required

Do not infer an FPS improvement from fixture success. Compare the same ARM64 driver/UMD
package at window 1/32/64/128 on the target device. Record CPU frame time, frame-time tails,
throughput, aggregate counts, memory growth and errors across cold/warm shader-cache runs.
Exercise allocation churn, multiple Vulkan queues, repeated mode changes, suspend/resume,
TDR/reset and process exit under GPU load. Use Driver Verifier only on a recoverable test
installation. Keep identical Mesa/DXVK/VKD3D revisions and configuration between controls.

Deferred BO destruction, IOVA quarantine, monitored-fence waits, all-live-BO dependency
tracking and generic DXVK/VKD3D hot-path edits are **not implemented by this patch**.
They require their own lifecycle/ABI review and target workload evidence; existing BO and
allocation references are deliberately retained until their established terminal paths.
