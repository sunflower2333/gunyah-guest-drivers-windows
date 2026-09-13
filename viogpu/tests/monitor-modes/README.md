# Monitor-mode enumeration callback

Run `python viogpu/tests/monitor-modes/run.py`. The runner extracts the actual
`VioGpuDod::AddSingleMonitorMode` and `BuildVideoSignalInfo` definitions and
compiles them against controlled Windows callback peers. The production
`display_timing.h` supplies rational conversion. Linux uses ASAN/UBSAN; Windows
uses MSVC `/W4 /WX` as part of the complete ARM64 WDK workflow.

The 95 checks cover a preexisting preferred60Hz mode with missing165Hz and
1920x1080 alternatives, later duplicates, first/later add or create failures,
release failures, initiating-error preservation and ownership transfer. Signal
checks retain3040x1904 with1160680000Hz pixel rate,1450850/8793Hz vertical
frequency and2901700/9Hz horizontal frequency. Both current60 and current165
preference policies are exercised; no mode is applied.

`--negative-control-duplicate` restores the premature successful return and
must detect missing modes. `--negative-control-add-error` restores error
masking and must detect loss of the failed-add status. Each negative succeeds
only when the intended semantic failure is observed. The unmodified875edab
callback failed five checks before the production repair.

These are controlled callback tests, not evidence of actual Windows165Hz mode
switching, delivery cadence, or recovery from the previously observed host
synchronous timeout.
