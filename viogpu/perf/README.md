# Native-submit measurement analysis

This tool consumes the existing three-line `viogpu perf:` diagnostic. It does not
add per-submit logging, change driver/UMD/host ABI, or change BO/IOVA retirement.
It closes the measurement side of the submit-window experiment, not hardware acceptance.

## Capture and compare

Capture UTF-8 debugger output for **one adapter lifetime per file**, with the same
workload, package revisions, shader-cache state and settings in each A/B run.
Use window 1 as the serialized control; compare 32, 64 or 128 independently.
Keep the Mesa, DXVK, VKD3D and KMD revision/package identifiers with the capture.
The log format does not identify the adapter: indistinguishably interleaved
adapter records cannot be reliably detected, so they must not be mixed.

The reporter emits only at an existing successful drain after new work. Obtain
at least two distinct drained snapshots around the observed interval. The first
snapshot is the baseline; the tool does not assume the counters started at zero
when capture began. A single snapshot produces cumulative data but no interval
or A/B result. Do not insert per-submit drains just to collect metrics: that
would change the workload and defeat the pipeline experiment.

```sh
python viogpu/perf/analyze_submit_perf.py window1.log --output window1.json
python viogpu/perf/analyze_submit_perf.py window1.log --compare window64.log --output comparison.json
python -m unittest discover -s viogpu/perf -p 'test_*.py' -v
```

JSON contains exact integer counter deltas, weighted references/bytes per Render,
mean dispatch-to-terminal duration in microseconds, and the pipeline-handoff
fraction. A zero denominator is `null`, not zero or infinity. Duplicate snapshots
are counted but do not add work. Files are parsed successfully before JSON is
written, and an output path cannot overwrite either input capture.

## Interpretation and rejected input

- `host_pending_peak` includes software backlog, not just physical-host inflight.
- Dispatch-to-terminal time includes validation and queue delay, not GPU-only time.
- Lifetime maxima stay lifetime maxima; their differences are not interval maxima.
- A/B output is an observation, not an FPS, throughput, utilization or latency-tail
  estimate. There is no wall-time or per-event timing distribution in these records.
  A lower mean retirement time can accompany lower throughput; profile the target.

Malformed, incomplete, interleaved and orphan metric rows fail with exit code 2.
The tool verifies fully drained work accounting. It also rejects mixed windows,
decreasing counters/maxima, overflow, and inconsistent interval accounting.
Counter reset/wrap, reordered capture and mixed adapters are not automatically
interpreted as a new epoch: split the source log into known runs and rerun.
Unknown fields fail so format changes require an explicit schema/test update.
Unrelated debugger lines and prefixes are allowed. UTF-16 exports must first be
converted to UTF-8 without losing complete records.

The tests use synthetic data and check the field names against the production
reporter. Their success demonstrates parsing/accounting behavior, not a measured
GPU speedup. Deferred BO destruction, IOVA quarantine and hardware validation
remain separate work; this tool does not weaken their lifetime requirements.
