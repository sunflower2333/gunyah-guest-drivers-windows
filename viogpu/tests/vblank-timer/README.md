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
It covers a multi-frame stall with preserved raster phase and skipped whole frames,
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

The cadence diagnostics fixture also executes actual recording, capture and
registry publication bodies. It checks fractional and exact-period resyncs,
early/invalid callbacks, all five delivery outcomes, an in-flight delivery,
arm/disarm idempotence, mode transitions, capture/publication concurrency and
registry-write failure. A stable grid obeys the exact conservation equation
`delta NextDueTicks = PeriodTicks * delta DueCount + delta ResyncPhaseTicks`.
The `--negative-control-phase-grid` restores the old late `now + period`
restart and must fail the stalled-raster check. Four additional semantic
negatives must fail on their respective diagnostic:
`--negative-control-cadence-resync`, `--negative-control-cadence-gates`,
`--negative-control-cadence-snapshot`, `--negative-control-cadence-publish`.
`source_contract_test.py` also rejects pageable lock holders and torn/partial
snapshot publication. Both workflows require those cases and the new .text MAP
symbols. No WDK/MAP execution or device result is implied by host tests.

`vblank_cadence.h` defines the fixed208-byte, little-endian schema1 REG_BINARY
value `NativeVblankCadenceSnapshot`, published by the existing synchronous
allocation diagnostic trigger. The old registry DWORDs remain compatible.
Compare this binary value's own SnapshotQpc values, not a user-mode sleep or
timestamps from the older independently published DWORDs. Require equal
AdapterStartQpc/QpcFrequency/mode identity/PeriodTicks, no change in ArmCount,
DisarmCount or ModeChanges, and enabled/armed valid endpoints. ArmCount counts
transitions including attempts that subsequently fail; ModeChanges counts
accepted timing publications including ones rolled back after arm failure.

CallbackCount = DueCount + EarlyCount + InvalidPeriodCount. DueCount minus the
sum of Delivery outcomes is the number between Due and outcome recording; it
can straddle a snapshot and must not be called a lost interrupt. Delivered is
the successful Notify wrapper count; in MPO2 mode an obsolete inner notification
can be suppressed, so it is not a physical-vsync/scanout measurement.
ResyncCount counts lateness of at least one period; MissedWholePeriods counts
its integer-period portion. ResyncPhaseTicks always counts actual NextDue
advance beyond one period per DueCount. Older `now + period` builds included
fractional phase loss; the grid-preserving policy counts only skipped whole
periods, so ResyncPhaseTicks equals PeriodTicks * MissedWholePeriods within a
stable-mode window. The wire layout and exact conservation equation are
unchanged, and the existing reader accepts both policies. Interpret this field
against the exact driver revision; it is not raw callback lateness.
InvalidPeriodCount also counts rejection when no signed future grid deadline
is representable; EarlyCount excludes that failure. MaxLatenessTicks is a lifetime
maximum, not a per-window maximum. SnapshotQpc must be fresh and increasing;
atomic registry values may still be replaced by an older concurrent publisher.

64 forced races pause a real production callback either before its rearm call
or during delivery. Concurrent disarm must disable and wait for that callback;
concurrent arm cannot publish the next timer before old callback rundown.
Accepted rearm before cancellation and disabled rearm during cancellation are
both covered, as are allocation failure, invalid timing/QPC, and early wakeup.
The actual delivery body observes enable/disable and hardware interrupt gates.
A multi-frame stall delivers only one vblank and skips to the first strictly
future point on the same grid; an immediate
extra callback cannot produce a catchup burst. The full delivery body compiles
with the MPO path enabled; OS notification and worker signaling remain peers.

`tests/display-timing/display_timing_test.cpp` additionally covers 100 seconds
at 20/60/120/144/165/240/360 Hz with 10/19.2/24 MHz QPC, conversion rounding,
saturation, 128466 exhaustive small signed-range grid cases, 17 signed-limit
cases (including unrepresentable deadlines), and the long-stall/no-burst rule.
The extracted driver additionally checks 2000 mixed whole/fractional stalls,
unchanged diagnostic conservation, correct invalid/early classification and
full-width unsigned lateness without signed subtraction overflow.

These are source and host-model checks. They do not prove Windows timer
implementation behavior, new binary section placement, guest CPU savings,
real display cadence, suspend/resume, or device reset. The adapter worker's
independent ExSetTimerResolution(5000, TRUE) remains unchanged.
