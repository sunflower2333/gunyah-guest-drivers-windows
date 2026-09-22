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

The KMD now uses these ownership paths for HostSurface scanout. These tests
establish transport and software ownership behavior, not Android visibility or
end-to-end zero-copy runtime. The existing same-AHB BAR mapping still needs the
host/backend's actual guest mapping support and sufficient non-overlapping BAR
space. No WDK/runtime claim follows from these portable fixtures.
