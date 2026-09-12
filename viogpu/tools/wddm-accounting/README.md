# WDDM accounting capture

Builds a native ARM64 console tool against the real Microsoft SDK/WDK and a
PowerShell collector for an explicitly selected adapter and existing short GPU
workload. No driver installation, VM operation or workload download occurs.

First identify adapters with `./wddm-accounting.exe`. It prints JSON containing
adapter LUID, DXGI description/vendor/device, KMT driver model version, actual
render/display/software flags, segment memory totals and node metadata. Failed
queries retain their NTSTATUS. Node metadata probes physical adapter 0 and stop
at the first failure or 64 nodes; this discovered list is not a guaranteed node
count. The raw driver model uses Microsoft `D3DKMT_DRIVERVERSION` values, for
example 2000 means WDDM 2.0. It is distinct from the package file version.

Example, replacing the LUID, executable and arguments with the actual reviewed
short workload on the existing VM:

```powershell
./collect-wddm-accounting.ps1 -AdapterLuid 0x00000000_0x00012345 `
  -WorkloadPath C:\gpu-tests\existing-short-test.exe `
  -WorkloadArguments '--frames 120' -WorkloadTimeoutSeconds 15
```

`WorkloadArguments` is the exact Windows argument string; quote arguments inside
that string as required by the selected application. Use a workload that exits
after a bounded amount of correct GPU work. Avoid a browser launcher that hands
the work to an existing process. This script starts and, on timeout, terminates
only its own workload process object. It records the workload hash, PID, exit
code, timeout and stdout/stderr. It never kills a pre-existing application.

The collector samples idle-before, workload, and idle-after, normally at about
one-second intervals plus query time. Each native probe has a five-second
deadline; each CIM call requests a three-second operation timeout. A workload
deadline is checked between samples, so a slow sample adds query overhead to
the requested limit. These timeouts do not guarantee recovery from a kernel
or device hang. Default capture is 5 seconds idle, at most 15 seconds workload,
then 5 seconds idle, plus query overhead.

Public performance counter providers supply `GPU Engine`, `GPU Adapter Memory`
and `GPU Process Memory` through language-independent raw CIM class names.
Rows are selected by the exact adapter LUID. Engine instances retain the PID
and engine identity in their names. Engine utilization is the delta in raw
100-ns running time divided by the actual 100-ns time interval. Unknown rows,
provider failures and counter resets are not zero usage; utilization above
100 percent is retained and flagged, not clamped. Memory releases are signed
deltas. Missing instances invalidate the baseline rather than bridging gaps.

For diagnostic-only per-node and per-segment KMT raw statistics, opt in with
`-KmtStatistics` (native switch `--kmt-statistics`). Microsoft explicitly marks
`D3DKMT_QUERYSTATISTICS` as system-reserved, so this optional reader may fail or
change with Windows/WDK versions. It uses actual WDK declarations, preserves
each NTSTATUS, caps counts at 64, and cannot stand in for the public providers.
Its segment index is the statistics-array index, not a claimed KMD SegmentId.
The driver does not implement a corresponding private or reserved query.

Outputs stay in a fresh local capture directory: `samples.jsonl`,
`summary.json`, and workload stdout/stderr. `summary.json` records DWM/Explorer
PIDs before and after and file hashes for the probe/workload. The caller must
also record exact loaded KMD/UMD/ICD identity and check event logs/dumps.

Exit 0 means the collection and workload exited without a detected collection
error. It does **not** mean the GPU goal passed. Empty providers, no process
attribution, an unexpected adapter or a flat utilization graph need review.
Acceptance requires changing idle/load/idle counters, plausible shared-memory
accounting and release, correct completed workload output, no new TDR/crash,
and a stable visible Full VIOGPU desktop. Actual Task Manager UI inspection is
still required. VidSch busy time is not a physical Adreno utilization sensor;
compare host KGSL/CPU samples for efficiency claims.

Official references:

- [KMT query types](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ne-d3dkmthk-_kmtqueryadapterinfotype)
- [Node metadata](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-d3dkmt_nodemetadata)
- [Segment sizes](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_segmentsizeinfo)
- [Reserved query-statistics ABI](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-d3dkmt_querystatistics)
- [Task Manager uses VidSch/VidMm](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/)

The standalone CI workflow compiles/links ARM64, validates its PE machine and
startup, parses the scripts, executes 10 delta edge cases, and packages the
tool/scripts. It does not claim GPU runtime validation on a CI runner.
