# Synchronous control timeout regression

`python3 viogpu/tests/synchronous-timeout/run.py` compiles and runs the actual
production submit, poison, re-enable, teardown, timeout decoder, publisher and
reader functions against controlled OS/queue peers. The wire definitions and
diagnostic structure are extracted from production headers. Linux enables
ASan/UBSan; CI also runs the fixture with MSVC before the ARM64 WDK build.

The fixture checks accepted/rejected submissions, successful completion,
completion epoch mismatch, submitted timeout, poisoned admission, mutex-only
failure, recovery followed by another submitted failure, and concurrent readers
of the immutable first snapshot. All truncated lengths for each of the eleven
resource commands use exactly sized, deliberately unaligned buffers. Unknown,
nonresource, null and invalid signed-length inputs never invent a resource ID.

`--negative-control-overwrite` removes first-writer admission in the extracted
production function. It must fail the exact assertion that the first submitted
failure survives teardown/re-enable; unrelated failures are not accepted.

These fixtures and the WDK build validate source behavior and compilation, not
host completion latency, physical-device rendering, or the 165 Hz mode change.

The mode-publication case compiles the actual `VioGpuDod::Flush2DResource`
wrapper, adapter flush leaf, failure diagnostic helper, and persistent registry
writer. It creates an actual production first-timeout snapshot and drives the
mode failure entry without destroying a context. Controlled peers verify exact
registry fields, partial-write invalidation/retry, balanced hardware/registry
ownership, no submission-gate reopening, no stale adapter access after release,
and no diagnostic IO for healthy or nonpassive paths. It preserves descriptor
quarantine and the unchanged five-second timeout.

`--negative-control-mode-publication` removes both publication calls from the
actual mode flush wrapper. It must fail the precise assertion that a failed
mode flush publishes the first timeout without waiting for context destruction.
