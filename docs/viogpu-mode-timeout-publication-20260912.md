# Mode failure timeout publication

The58472 modeset run reached a five-second flush failure, paging stage19 and
DWM loss, but NativeSynchronousTimeout registry fields remained absent. The
first timeout was already captured in the control queue. The only persistent
publisher was called during failed native-context destruction, which this
mode path did not reach. An absent registry value therefore did not establish
that no synchronous timeout occurred.

The actual mode flush wrapper now publishes failure diagnostics both after an
unconfirmed flush and when submission admission is already closed. It releases
the ordinary submission operation, then independently acquires hardware rundown
at PASSIVE_LEVEL for diagnostic access. Hardware teardown or a missing adapter
causes a safe skip. The adapter snapshot reader only publishes for an unhealthy
synchronous queue; the existing writer records the original immutable timeout
and commits validity after all fields are written. Later failures can retry a
registry write failure. No submit gate is reopened, wait extended, descriptor
released, queue unpoisoned, reset altered, or package version changed.

The production mode wrapper, adapter flush leaf, diagnostic helpers and registry
writer are executed by the extended synchronous-timeout fixture. Its negative
control removes the new publication calls and must fail on the original missing
mode-path behavior. Actual queue recording, first-failure retention, five-second
wait and quarantined-buffer ownership regressions remain in the same fixture.

The base signed workflow already pins58472 while its source contract still
expected58471. This descendant aligns that stale self-check literal with the
existing workflow. Parent chooses the next signed installation version.

This is diagnostic plumbing. CI does not demonstrate successful165Hz switching
or identify the host command that timed out; the next parent-owned target run
must read NativeSynchronousTimeoutValid, Type, ResourceId, ContextId, CallerRva,
WaitStatus and EpochGeneration from the active driver's registry key and
correlate them with the first failed flip/paging entry and crosvm logs.
