Run `python3 viogpu/tests/native-ahb-release/run.py --negative-controls`.

The fixture compiles the actual asynchronous CtrlQueue implementation, terminal
callback arbitration and wire structures with a deterministic host/queue shim.
It verifies an outstanding WAIT does not block a different PRESENT, strict
40-byte replies, resource/sequence/header/epoch checks, never-presented WAIT(0),
queue-full and allocation failures, reset cancellation, and completion before
enqueue returns. ASan/UBSan checks ownership and storage lifetime; six mutations
prove malformed acknowledgements and cancellation cannot grant release.

Run `python3 viogpu/tests/native-ahb-release/ownership.py` for the extracted
production registry admission, producer retirement, authenticated host BO
repacking and scheduler barrier fixtures. Negative controls cover forged IOVA,
unknown GPU completion, missing release observation, blocked next Present,
same-context timestamp reordering and omitted host residency.

The held-front case also queues a future GPU writer. Repeating the unchanged
front must preserve that writer's pin without granting access or waiting for
the displayed buffer to release itself. The writer is admitted only after a
real release. A count-only pending-writer guard fails the negative control.
The shim tracks spin locks and rejects waits while one is held.

Activity reattach refresh uses a new guest reservation and an asynchronous
`REFRESH_NATIVE_AHB` request carrying the last accepted sequence. An unchanged
reply is not a release; only `WAIT_NATIVE_AHB_RELEASE` may advance reusable
ownership. The production ownership fixture covers producer deferral, blocked
read/write admission, coalesced retry, ordinary-present races, enqueue failure
and uncertain reset completion. Four refresh ownership mutations and one stale
refresh response mutation must fail. Both ARM64 workflows execute these Linux
fixtures and require the callback/wakeup entry points in the final MAP's `.text`.

Detached paging cases run later pageout/discard before the initial FILL and
require FIFO for overlapping allocation sets. They also allow independent
paging, Render and replacement Present while another allocation waits for
Android release. Removing the overlap guard must fail the interleaving fixture.

The KMD now uses these ownership paths for HostSurface scanout. These tests
establish transport and software ownership behavior, not Android visibility or
end-to-end zero-copy runtime. The existing same-AHB BAR mapping still needs the
host/backend's actual guest mapping support and sufficient non-overlapping BAR
space. No WDK/runtime claim follows from these portable fixtures.
