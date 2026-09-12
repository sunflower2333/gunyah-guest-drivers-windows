# Versioned display mode helper regression

Run `python3 viogpu/tests/display-mode-helper/run.py` locally with Mono, or
`python viogpu/tests/display-mode-helper/run.py` on Windows with the installed
.NET Framework C# compiler. The runner extracts the actual embedded production
C# from `.install_scripts/viogpu-test-display-mode-v2.ps1` and supplies controlled
display API peers. No user32 entry, display change or sleep is executed.

Coverage includes failed apply with changed and unchanged current mode,
restoration failure or exception, successful roundtrip, reported restore
success without matching state, failed return with restored state, unknown
current display state, failed save, hold failure/drift, test-only operation,
and preserving original position, orientation and bit depth.

The helper makes one restoration call when current state differs from the
snapshot or cannot be read. It observes current state even after failed apply
and never suppresses the original error when restoration succeeds or fails.
The bound is the number of calls, not a new timeout on the synchronous Windows
API. Killing a still-running apply and starting a competing restore is avoided.

Two semantic negatives run separately:

```text
python3 viogpu/tests/display-mode-helper/run.py --negative-control-success-gate
python3 viogpu/tests/display-mode-helper/run.py --negative-control-mask-failure
```

The first reinstates restoration admission based on successful apply, and must
fail `failed apply with changed mode restores exactly once`. The second replaces
the initiating failure with the restoration result, and must fail
`restore failure preserves original apply failure and both outcomes`.

The versioned script can be transferred and invoked like the original helper.
The original script, KMD, signed workflow and installed package are unchanged.
The helper only restores active display state with flags 0; it does not prove
165 Hz works, recover a crashed VM, or roll back persistent settings from an
unsuccessful `-Keep` save. Use the default temporary roundtrip for diagnostics.
