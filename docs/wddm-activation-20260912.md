# WDDM physical-mode activation candidate

This independent candidate integrates the physical WDDM2 allocation, node and
VidMm segment contracts into primary7eb8bff (current package58453 source plus
tests), preserving Mesa668d598 and the current OpenGL/OpenCL workflows. Candidate
version58460 is reserved by the main integrator. It does not enable GPUVA/MMU,
invent independent engines or dedicated VRAM, or implement native D3D12 admission.

The prior WDDM2 work used an older58373 base. This integration reuses only its
physical allocation flag union, QuerySegment4/GetNodeMetadata, matching table
registration and tests. TDR POST-ownership exclusion, resident/noinline timing,
checked D0 recovery and failed-packet scheduler-TDR policies in the newer
baseline are retained. The abandoned partial display-timings backport is not an
input. All source work is in an independent depth1 local clone and branch
`viogpu-wddm-activation-20260912`; active parent sources and device are untouched.

## Runtime model

One 3D node represents the existing shared graphics/compute/transfer submission
path. Native allocations use actual `FlagsWddm2.AccessedPhysically` and physical
adapter0. There is one CPU-visible, coherent aperture backed by VidMm-managed
guest RAM. Its address extent is not dedicated VRAM. GPUVA, GpuMmu and IoMmu
remain disabled. Existing allocation/patch lists, paging, Render/Submit, fence
publication and reset lifetimes remain the implementation.

Microsoft's `gpu-virtual-memory-in-wddm-2-0.md` expressly allows physical-mode
WDDM2 engines. `gpu-segments.md` requires AccessedPhysically for allocation-list
access and describes the implicit system-memory segment0. The public segment4
count query defines only NbSegment; this implementation preserves all other
members and a future descriptor-stride tail. Reserved physical-adapter query
types are not implemented. Task Manager uses VidSch/VidMm accounting; its graph
does not directly measure the Android Adreno sensor.

## Activation evidence

`NativeActivationTrace` version2 is one pointer-free4288-byte REG_BINARY value.
Each StartDevice attempt advances `NativeActivationEpoch` by2. The recorder
writes an odd invalid marker, a fresh even-epoch snapshot, then commits the even
marker. A partially written initialization stays invalid; epoch overflow disables
recording instead of reusing an identity. Registry errors are logged and, when
possible, published as `NativeActivationWriteStatus`.

The snapshot records original StartDevice stage/status/detail and first startup
failure, separate from later D0 transport stages. Phase1=starting,2=active,
3=stopping,4=stopped,5=start failure,6=unwinding. All QueryAdapterInfo completions
are counted; the first64 are retained. Separate first/last failed-query entries
continue updating after64, so later teardown cannot erase the original refusal.
Each entry contains type/status/sizes, phase, lifecycle flags and sampled
readiness mask. Failed-query output is never read. Successful DriverCaps fields
are size-gated; segment4 reads only NbSegment. Hardware snapshots acquire the
existing rundown protection before accessing the hardware adapter.

Read the exact active adapter's driver key with:

```powershell
./Tools/read-native-activation-trace.ps1 -RegistryPath '<active adapter driver key>'
```

The decoder rejects stale/odd epochs, failed write markers, invalid extents,
counts, failure identities and undefined failed-query output. A registry access
failure can prevent even the diagnostic error marker from being updated; event
logs and exact installed package identity remain necessary to establish that a
snapshot belongs to the start under investigation. Legacy NativeStartStage and
NativeReadinessFailMask values remain available but are not current-session
proof. A refused optional query and a sampled readiness mask are observations,
not automatic root-cause attribution.

## Validation and remaining gates

Local full-miniport source contract,17 production WDDM2 accounting cases and198
adapter identity assertions passed. The actual production trace recorder passed
590 registry, startup, saturation, failed-output and hardware-lifetime assertions
under ASAN/UBSAN. Semantic negative controls reject a reused epoch and loss of
the first failure after64 queries. Windows CI additionally executes the query,
allocation and metadata functions against actual WDK declarations, runs the
PowerShell decoder regressions, and compiles/links the complete ARM64 miniport.
Final CI outcomes and exact artifact identities are recorded by the handoff.

Use `viogpu/tools/wddm-accounting/` to capture actual D3DKMT node/segment/process
statistics before, during and after a bounded workload. The standalone tool
requires exact adapter selection; process and allocation lifetimes remain real
VidSch/VidMm measurements. Compile-only, mocked-registry and CPU tests are not
Task Manager or target GPU validation.

The parent owns device installation/recovery and must first restore a healthy
single existing VM. Required target gates are adapter status0, actual WDDM2
enumeration, stable Full VIOGPU output, Task Manager GPU visibility with changing
engine and shared-memory counters, and repeated Vulkan/D3D load plus idle/reset
recovery. Prior58422 Code43 and the currently unhealthy58337 are not attributed
to a newly found defect by this implementation.
