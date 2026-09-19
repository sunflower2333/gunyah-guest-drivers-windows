# Primary pixel read cache

The standard primary is deliberately not `CpuVisible` or `Cached`. The Windows
allocation contract forbids `Cached` on primary surfaces and rejects UMD
`LockCb` on non-CPU-visible allocations. This change preserves that contract.

The KMD can retain a private cached copy of pixels after a full Present/copy
overwrite. The existing scheduled transaction and both allocation lifecycle
mutexes still govern reads and writes. Host scanout continues to use the real
primary backing, which contains the published pixels before completion as before.
For an already valid private mirror, equal raw-format rows need no repeated
backing write. Every byte of the requested row is compared; converted rows and
invalid/external-writer surfaces are never elided. Changed rows update the mirror
first, then copy those final bytes into backing, reading/converting the source
only once. Fences, scheduling, cache flushes and Host publication are unchanged.
`NativeCopyKiB` counts bytes actually written to the destination, so an unchanged
copy may complete successfully with zero copied bytes.

Writer and invalidation inventory in `wddmddi.cpp`:

| Path | Cache action |
| --- | --- |
| `ExecutePresentTransaction` full overwrite | Establish pixel validity from source, including conversion |
| Same path, partial rectangle(s) | Maintain an already valid cache; cannot establish one |
| `CopyAperturePlacement` page-in | Invalidate before writing |
| `FillAperturePlacement` | Invalidate before writing |
| Page-out | Read actual backing; preserve cache validity |
| `ReleaseApertureCpuMapping`, including PFN remap/unmap | Free and invalidate |
| Host reset generation changes | Reject cached reads until a new full overwrite |
| Native GPU allocations or CPU-visible GDI/staging | Never eligible |

Primary and native flags cannot coexist (`ValidateAllocationPrivate`); native
render references require `IsNativeAllocation`. Standard primary backing is
therefore not a native GPU command target. Adding any new primary writer must
extend this inventory or invalidate the cache before that writer becomes active.
Do not extend this cache to CPU-visible surfaces without tracking external writes.

Only validated pixel spans are read from the cache; pitch padding is deliberately
not cached or used for paging. Allocation size is capped at 32 MiB, with at most
four live/reserved caches across adapters. Pool exhaustion/allocation failure
falls back to the backing without failing the transaction. No extra allocation
is attempted for an invalid partial overwrite.

`NativePrimaryCacheReads` and `NativePrimaryCacheWrites` count cached Present
reads and mirrored Present writes in the existing explicit diagnostic snapshot.
They are not GPU utilization counters.

Run `python3 viogpu/tests/primary-read-cache/run.py` on Linux. The harness
compiles the actual production helpers and actual Present rectangle loop under
ASan/UBSan. It checks colors against an independent byte oracle, padding,
partial-write coherence, readback, reset/paging/fill invalidation, allocation
failure, CPU-visible exclusion and pool accounting, unchanged rows, last-byte
changes and publication after reset. Five semantic negative
controls must fail. Existing Present format tests still exercise both WDDM
interfaces and the row conversion matrix.

This is a CPU-copy optimization candidate, not native zero-copy scanout.
Windows compilation, signed package identity and device functional/FPS tests
are required before runtime acceptance. Do not reuse the base 58557 version
for a deployable unchanged-row candidate.
