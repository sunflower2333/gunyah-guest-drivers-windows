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

The fixture also runs both production quiesce functions with native, adapter,
and simultaneous mutex timeouts. Teardown must fail the caller's `NT_SUCCESS`
check unless both mutexes actually drained; `STATUS_TIMEOUT` is a positive
status and must never be returned as successful quiescence. An offline adapter
channel cannot hide a still-active native waiter. A later successful retry may
drain both poisoned channels and finish teardown.

`--negative-control-quiesce native-result`, `native-timeout`, and
`adapter-timeout` independently restore the discarded native status or either
raw positive timeout. Each must fail its specific teardown-safety assertion.
