# One-shot vblank timer host regression

Run `python3 viogpu/tests/vblank-timer/run.py` on a Linux host with C++17,
or `python viogpu/tests/vblank-timer/run.py` from an MSVC developer shell.
The runner extracts the actual timer callback and Arm/Due/Rearm/Deliver/Disarm,
GetScanLine and SetCrtcTiming bodies
from viogpudo.cpp, compiles them with ASan/UBSan on Linux, and supplies an EX_TIMER peer
that models Microsoft's documented disable/cancel/callback-wait contract.

The production methods deliver 16500 vblanks in a simulated 100 seconds at
165 Hz with a 19.2 MHz QPC, 0.5 ms expiry quantization, and variable callback
work. A second profile adds another fixed 500 us callback delay and also
delivers 16500 frames. Both require 16500 callbacks instead of the prior
sampler's nominal 200000.
The negative control `--negative-control-relative` replaces the absolute QPC
deadline with a new full-frame delay after each callback; the test detects the
resulting 15384-frame drift.
The `--negative-control-enable` removes the actual delivery enable check and
is rejected when a disabled interrupt incorrectly delivers a vblank. Both
controls require their specific semantic failure, so an unrelated crash or
compile failure is not accepted as success.

The raster regression reads actual GetScanLine output before/at/after a QPC
deadline and after late delivery. Armed phase follows the timer deadline even
when Deliver changes its callback epoch; disabled queries retain epoch behavior.
It covers a multi-frame stall and the existing timer's deliberate resynchronization,
mode changes, invalid modes, allocation-failure rollback, and the temporary
not-ready state before Arm publishes its new grid. An injected mode change just
after the query releases its timing lock verifies the entire old snapshot is
used without mixing a new deadline/period/geometry into it.

Three semantic negative controls each require a specific failure:

- `--negative-control-scanline-epoch` restores callback-relative raster phase.
- `--negative-control-scanline-arm` leaves the previous mode's deadline live
  during the arm transition.
- `--negative-control-scanline-snapshot` reads the deadline after unlocking,
  mixing a concurrent mode change with the old snapshot.

All are host peers of the extracted production bodies. Timing-lock interleavings
are deterministic boundary injections; the kernel, Android VSYNC, and physical
scanout are not emulated or certified.

64 forced races pause a real production callback either before its rearm call
or during delivery. Concurrent disarm must disable and wait for that callback;
concurrent arm cannot publish the next timer before old callback rundown.
Accepted rearm before cancellation and disabled rearm during cancellation are
both covered, as are allocation failure, invalid timing/QPC, and early wakeup.
The actual delivery body observes enable/disable and hardware interrupt gates.
A multi-frame stall delivers only one vblank and resynchronizes; an immediate
extra callback cannot produce a catchup burst. The full delivery body compiles
with the MPO path enabled; OS notification and worker signaling remain peers.

`tests/display-timing/display_timing_test.cpp` additionally covers 100 seconds
at 20/60/120/144/165/240/360 Hz with 10/19.2/24 MHz QPC, conversion rounding,
saturation, and the existing long-stall/no-burst rule.

These are source and host-model checks. They do not prove Windows timer
implementation behavior, new binary section placement, guest CPU savings,
real display cadence, suspend/resume, or device reset. The adapter worker's
independent ExSetTimerResolution(5000, TRUE) remains unchanged.
