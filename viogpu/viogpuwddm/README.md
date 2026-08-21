# viogpuwddm compile-only skeleton

This project type-checks a full-miniport-shaped WDDM 1.2 callback table for the
VirtIO GPU 3D path. It is only a compile/link skeleton, not a complete WDDM 1.2
implementation, and is intentionally not installable:

- `DriverEntry` returns `STATUS_NOT_SUPPORTED` without calling the registration
  helper.
- There is no INF, package target, or signing input.
- Native ring-1 fence retirement and reset-generation/TDR source contracts are
  wired, but the target still does not advertise hardware preemption and is not
  a product-grade recovery implementation.

## Native Context scope

The current artifact extends the P1 compile-only transport-readiness slice with
a P2 pre-v1 private-ABI validation scaffold. Its unreachable source path
negotiates the required VirtIO-GPU features, validates the Native Context
capset, exposes an exact-revision WDK-independent UMD/KMD snapshot, creates a
Host Native Context and its blob-0 control resource, obtains the context VA
range through exact `GET_PARAM` requests, and validates allocation, context,
Escape, and Render identities against a stable reset generation. The source
path also covers guest-backed VidMm placement and Host blob ownership,
Patch/SubmitCommand, ring-1 fence publication and retirement, and retry-safe
allocation teardown. These remain compile-only source contracts: Host GPU
execution, Windows loading, runtime fence retirement, Present, and pool
behavior are unverified. `DriverEntry` remains fail-closed and returns
`STATUS_NOT_SUPPORTED`; no artifact from this project may be installed or
loaded.

The fixture in `viogpu/tests/wddm-private-abi/` locks only the current pre-v1
snapshot for independent KMD and UMD endpoint regression checks. It does not
freeze a version-1 ABI. The exact-revision snapshot now defines a low-frequency,
context-scoped `D3DKMTEscape(DRIVERPRIVATE)` `GET_CONTEXT_INFO` request. The KMD
requires the exact buffer layout, zero input output fields, matching adapter,
device, context, and reset generation, and holds both context rundown and the
protected Native Context snapshot through response publication. It exposes
`VaStart`, `VaSize`, `ResetGeneration`, and an opaque per-context `ContextId`
token that the UMD must echo in native allocation private data; KMD pointers,
Windows handles, and VirtIO, resource, blob, or KGSL identifiers remain private.

The source path now obtains each renderer context's range through exact
`MSM_PARAM_VA_START` and `MSM_PARAM_VA_SIZE` requests. It creates a 16 KiB
Host3D blob with blob id zero, maps only a validated cacheable `drm2kgsl_host`
pool offset, seeds the bounded response slot, releases the pool lease before
the synchronous VirtIO submit, and reacquires the same pool generation to
consume an exact sequence and response layout. Malformed offsets, responses,
generations, faults, or VA ranges poison the transport. Only after both values
pass page-alignment and overflow checks are `VaStart` and `VaSize` published;
failure, destroy, and reset clear them. `GET_CONTEXT_INFO` therefore exposes the
Host-created context slice rather than substituting the adapter-wide capset
range. Before version 1 can be published, the requested-IOVA
bind/unbind/query semantics and guest-backed allocation/teardown ABI still need
device-runtime validation and a frozen contract. The UMD must query and cache
the response before selecting any BO IOVA or submitting, then discard it when
the generation changes. Escape is not a submit path. Create-time private data
is UMD-to-KMD input and must not be treated as an output channel. D3DKMT
allocation and context handles remain the UMD's opaque identities.

Except for the context-info response fields described above, the snapshot
contains no Windows pointers or handles, physical or IOVA addresses, or
VirtIO/KGSL object identifiers. Native allocation creation requires the
context token and reserves a non-overlapping requested-IOVA interval for that
registration. `CreateAllocation` retains the validated private
data and `OpenAllocation` requires an exact byte-for-byte match. Every open
wrapper holds its owning device through `CloseAllocation`, records read-only
access, and is rejected if a Render context from another device references it.
Render ranges are bounded by the logical UMD-declared allocation size, not
page-aligned VidMm backing padding. Open/close and destroy also reject
unsupported flags, resource-private data, and duplicate handle arrays before
releasing objects. `CreateContext` accepts only the single-engine affinity mask
`1`. A Native context uses flags zero plus the exact current private-data and
nonzero reset-generation token. Exact System and GDI context flags are also
accepted without private data; they do not create a Host Native Context. Native
`Render` bounds command input to 64 KiB, copies command and patch inputs to
nonpaged snapshots, validates their shared allocation identity, rejects writes
through read-only opens and overlapping 8-byte patch slots, rechecks the reset
generation, and only then publishes its DMA output. UMD-selected patch slot IDs
may be nonzero; reserved patch bits must be zero.

`Render` consumes every nonzero allocation-list `SegmentId` as prepatch input,
validates its current placement and Native Context ownership, and writes the
KMD-owned resource ID and requested IOVA while still returning the complete
patch list. `Patch` can repatch every reference after paging; when all references
were prepatched, `SubmitCommand` may legally receive the prepared command without
an intervening Patch call. It queues the native command and reports completion
through the ring-1 fence path, but this remains compile-only: Host GPU retirement,
Windows loading, and WDDM runtime behavior are unverified. The validation slice
must not be described as a working or installable driver.

The product baseline is `udmabuf=true` with independent `drm-host` and
`gpu-guest` boot pools. The product transport must fail closed unless
`VIRTIO_GPU_F_VIRGL`, `VIRTIO_GPU_F_CREATE_GUEST_HANDLE` (feature bit 6),
`VIRTIO_GPU_F_RESOURCE_BLOB`, and `VIRTIO_GPU_F_CONTEXT_INIT` are offered and
acknowledged. GPU retirement uses the control-header fence and ring semantics;
there is no additional generic fence feature bit. For a guest-backed BO, the
KMD/UMD contract is:

- allocate the extent from `gpu-guest` and set `MSM_BO_GUEST_ALLOC`;
- issue `RESOURCE_CREATE_BLOB` with `VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST` and
  `VIRTIO_GPU_BLOB_FLAG_CREATE_GUEST_HANDLE`;
- provide SG entries/iovecs covering the guest-pool extent so crosvm can validate
  the grants and build the bounded udmabuf imported by drm2kgsl.

The `drm-host` pool owns native-context control/response shmem and is independent
of the guest BO pool. The compile-only source implements the blob-zero control
lifecycle and the guest-backed `RESOURCE_CREATE_BLOB` ownership path; it is
still not runtime proof of BO operation. The product path does not use
`NCTX_LEGACY_HOST_ALLOC`, runtime SHARE, pool-outside GPA backing, or a second
offset allocator. Unknown CreateBlob/UNREF results poison the generation rather
than guessing ownership. Runtime crosvm support, map/unmap behavior, and
teardown remain unverified; compile/link success here does not provide that
behavior.

The compile-only target now discovers exactly one `drm2kgsl_host` provider and
one `gpu_guest` provider through the same versioned direct interface. It connects
`drm2kgsl_host` and `gpu_guest`, in that order, before VirtIO initialization.
Readiness and generation checks require both named pools. The discovery IOCTL
exposes only name, GPA, and size. Provider and client rundown protection make a
pool's kernel VA valid only inside a successful
`AcquireMapping`/`ReleaseMapping` interval, and provider `ReleaseHardware` waits
for those intervals before unmapping.

The full target's single CPU-visible VidMm segment is published from the exact
`gpu_guest` physical range. The stable Display-Only target retains its existing
nonpaged/contiguous fallback. The compile-only full target now has source paths
for VidMm placement, guest-backed blob creation, requested IOVA ownership, and
allocation teardown, but none has device-runtime proof.

The client now registers `EventCategoryTargetDeviceChange` with the callback's
own viogpu `DRIVER_OBJECT`. An active `drm2kgsl_host` or `gpu_guest` pool vetoes
query-remove with `STATUS_UNSUCCESSFUL`; the matching remove-cancelled event is
therefore a no-op. This conservative compile-only policy prevents VidMm from
retaining a `gpu_guest` segment after an orderly provider removal.

Remove-complete also covers surprise removal. Its callback atomically withdraws
pool readiness once, advances the pool generation, closes the outer hardware DDI
gate, poisons Native Context/reset generations, and queues a system work item.
The callback path does not acquire a mutex, wait for rundown, reopen the target,
or unregister synchronously. The worker and voluntary `Disconnect()` share the
single-flight blocking teardown: they drain mapping leases, use
`IoUnregisterPlugPlayNotificationEx`, drain callback rundown, and only then
release the direct interface and file reference. Explicit disconnect joins a
worker both before and after attempting teardown so the pool cannot be reused or
destroyed while a queued work item still owns it. The embedded work item is
initialized once for the pool lifetime. The worker's final pool access releases a
dedicated rundown reference; a PASSIVE_LEVEL waiter drains and reinitializes that
rundown before publishing reusable `Idle`. This is a last-object-access/lifetime
barrier based on the system worker dequeue contract, not a literal callback-return
barrier and not a general driver-image-unload proof. An unregister failure restores
every target owner and prevents `Offline` publication.

This remains a compile-only scaffold, not a loadable shared-memory transport.
The adapter mapping API now has one narrowly bounded blob-zero `GET_PARAM`
consumer. The current provider ABI
requires a cacheable ACPI resource and a short, non-suspendable mapping lease:
the client disables APCs for the whole lease, performs no waits or pageable
calls, and releases on the acquiring thread. Explicit disconnect and
surprise-remove teardown drain those leases outside the PnP callback. A retained
file or direct-interface reference is not a mapping lease and must not be treated
as removal coordination.

For `drm2kgsl_host`, the cache contract is Windows CPU mapping <-> crosvm/host
renderer CPU mapping through Gunyah Normal-WB, Inner-Shareable memory. KGSL
cache maintenance is a separate `gpu_guest` BO contract; the capset
`has_cached_coherent` bit does not authorize this control-ring mapping. The
blob-zero `GET_PARAM` path is the sole bounded mapping consumer and already
releases its seed lease before the VirtIO wait, then reacquires and validates the
same generation and sequence. General ring traffic remains blocked until its own
release/acquire publication points and bounded access path are implemented.

The compile-only Native Context target uses ordinary contiguous/nonpaged
fallback storage for VirtIO bookkeeping and does not acknowledge
`VIRTIO_F_ACCESS_PLATFORM`. Its `gpu_guest`/`drm2kgsl_host` named-pool path
remains compile-only until a product DMA mapping contract is proven. The target
still tears down fail-closed and retains its adapter on ownership proof
failure. This remains an explicit registration blocker alongside the missing
P2 device-runtime proof for guest-backed BO operation and teardown.

`VioGpuWddmBuildInitializationData` wires the existing display adapter,
interrupt, power, cursor, EDID, and VidPN lifecycle into a full-miniport
`DRIVER_INITIALIZATION_DATA` table. The separate
`VioGpuWddmInitializeMiniportCompileOnly` helper compiles and links the future
`DxgkInitialize` call, including trace initialization and failure cleanup, but
is deliberately unreachable from `DriverEntry`. The compile-only project sets
`OptimizeReferences=false` so the linker cannot discard that helper before
resolving its WDK contract. The helper has C linkage and is the project's sole
`ForceSymbolReferences` input, which keeps the unreachable function through
LTCG without adding a call site. On a future successful registration, the
registered `VioGpuDodUnload` callback owns trace cleanup. CI also checks that
the final linker map retains the helper from `driver_entry.obj` and resolves
`DxgkInitialize` from `displib:displib.obj`. `DxgkInitialize` is not expected to
remain in the linked driver's PE import table because `displib.lib` supplies a
static implementation; the stable display-only driver has the same behavior.

The inherited display callback implementation and the full-miniport entry point
share one WPP provider. `driver_entry.cpp` is the sole WPP initialization owner;
the project-local `wpp-non-owner.tpl` lets the reused `driver.cpp` compile its
trace calls and cleanup reference without emitting a second provider definition.

The callback table intentionally leaves these DDI slots unset:

- `DxgkDdiNotifyAcpiEvent` and `DxgkDdiControlEtwLogging`.
- `DxgkDdiSetPalette`, `DxgkDdiGetScanLine`, and `DxgkDdiControlInterrupt`.
- `DxgkDdiGetChildContainerId`.
- `DxgkDdiSetPowerComponentFState` and
  `DxgkDdiPowerRuntimeControlRequest`.

It also does not register the KMDOD-only `DxgkDdiPresentDisplayOnly` callback.
The full-miniport table now registers `DxgkDdiCollectDbgInfo` and the three
Windows 8 engine-TDR callbacks. Debug collection emits one bounded 32-byte
snapshot using atomic reset and fence queries. The engine callbacks validate
the single node/engine topology, but `SupportPerEngineTDR` remains zero:
Native Context has no Host primitive that can cancel or reset one engine.
`DxgkDdiResetEngine` therefore requests outer reset and returns failure so the
scheduler promotes recovery to the adapter-wide timeout path; this is not
per-engine TDR support.

Standard paging now covers primary, GDI shadow, and staging allocations. It
uses context-zero paging records, copies or fills the `gpu_guest` placement
under one mapping generation, keeps each allocation's low-range local resource
ID stable, and recreates primary 2D Host backing after a confirmed reset epoch.
`DxgkDdiSetVidPnSourceAddress` validates and selects only a resident standard
primary with current 2D backing.

The source also implements a synchronous CPU-copy `DxgkDdiPresent` path. A
Native context may present only an exact live native allocation identity; a GDI
context may present only a resident CPU-visible non-primary standard allocation
whose context and reset identity fields are zero. The destination must be a
resident standard primary with current 2D backing, opened writable. Build and
Patch validate bounded rectangles, allocation opens, placements, pool
generations, and patch records. Build uses each nonzero allocation-list
`SegmentId` to prepatch its placement while still returning both patch records;
Submit may skip Patch only when both source and destination were prepatched.
When Patch is required, Render and Present accept either a submission-relative
`PatchOffset` or the same offset adjusted by dxgkrnl to the full DMA buffer,
with checked addition against the submission start.
The private-data buffer is treated as uninitialized output until publication;
its input-only pointer and remaining-size fields are not advanced by multipass.
Present consumes at most 256 destination subrectangles per pass,
advances `MultipassOffset`, and returns
`STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER` until the original list is complete;
DMA or patch-list capacity exhaustion returns the same retry status without
publishing a transaction. Cancel accepts an owner only when both the complete
nonpaging private record and the exact DMA submission range match. Submit queues
a passive transaction that copies the selected rows in `gpu_guest`, issues the
2D transfer/flush, and reports the scheduler fence.
Built, Patched, Queued, and Executing transactions retain context, allocation,
adapter-registry, and operation references until one terminal retirement. Stop,
D-state, TDR, and system-display takeover close and drain the registry before
transport teardown. D0 reset recovery first closes and drains any transaction
left by an any-IRQL reset notification, then reopens publication only after the
Active reset epoch is proven. Reopening is idempotent in an already-open Active
epoch, while a failed closed-to-open transition restores the outer reset gate.
If an ordinary D1/D2/D3 hardware transition fails after the registry drain, the
registry remains closed and the outer reset gate is requested; only a later D0
recovery may publish a coherent Active epoch again.
The implementation phase completed before concentrated validation began. The
local contract checks, Python syntax checks, ABI fixtures, workflow YAML parse,
targeted clang-format gate, and diff check pass. ARM64 compile/link CI remains
pending. None of these checks are Windows/KMT/Host runtime proof, and they
cannot make the KMD loadable or installable.

The preemption callback validates the single node/engine contract, reports
`DXGK_INTERRUPT_DMA_PREEMPTED` only when the native fence queue is already
empty, and gates the adapter for TDR when work is in flight or the interrupt
notification cannot be synchronized. `PreemptionAware` deliberately remains
zero because Native Context has no Host cancellation/preemption primitive.
`DxgkDdiResetFromTimeout` performs the adapter-wide D3 transport teardown and
advances the reset fence only after teardown succeeds; restart reuses the
checked D0 recovery state machine. Render patch/submit and paging now use
bounded private records; paging transfers consume their MDL during
`DxgkDdiBuildPagingBuffer`. Each Built record acquires its allocation submission
reference before publication; Submit validates that existing owner instead of
acquiring it after VidMm has received the record. A cancellable passive worker
then publishes guest-pool placement and host BO ownership. Paging cancellation
signals every recognized transaction even after the passive worker owns the batch. A
structurally valid private range publishes its record count before deep record
validation so earlier recognizable owners can release allocation references if
a later record is stale or malformed.

`DXGKARG_SUBMITCOMMAND` does not expose a CPU DMA-buffer base. Render and
Present Submit therefore validate submission offsets and sizes against the
KMD-private owner's retained DMA pointer; paging resolves its retained packet
pointer directly. Patch and Cancel still use their supplied CPU DMA base for an
exact pointer-plus-offset identity check.

Submission-fault recovery no longer trusts the callback identity as a
prerequisite for reset. A zero fence or nonzero node/engine identity first
closes the outer hardware epoch, invalidates the pending native fence tracker,
and fails the inner Native Context transport. Only the scheduler's
`DXGK_INTERRUPT_DMA_FAULTED` notification is suppressed when its identity
cannot be represented legally.

The current source deliberately leaves four runtime assumptions unproven. A
Built paging record already owns the allocation, but still relies on VidMm
retaining the DMA buffer and KMD private record until Patch, SubmitCommand, or
CancelCommand. Each Present multipass retry is assumed to retain every
previously published private record until its corresponding submission retires.
CPU row copies and paging copies also assume cache coherence between the
Normal-WB `gpu_guest` mapping and Host KGSL access; memory barriers order CPU
accesses but do not add a platform CPU/GPU cache-maintenance primitive. Finally,
batch rollback clears matching in-progress paging state but cannot undo
placement or Host ownership already confirmed by an earlier executed record;
the ensuing scheduler fault and adapter reset are the only recovery boundary.
These are explicit runtime gates, and the current paths are compile-only source
evidence that does not prove preemption, TDR, paging, or Present behavior.
Mandatory full-graphics WDDM
1.2 semantics that are absent or unproven include video-memory offer/reclaim,
GPU preemption and FlipOnVSyncMmIo, per-engine TDR, optimized rotation, direct
flip, GDI hardware acceleration beyond the CPU-copy baseline, seamless state
transitions/PnP, and display container ID behavior. System contexts currently
exist only to satisfy typed lifecycle ownership; GDI contexts support the
bounded Present baseline, not a complete WDDM 1.2 GDI acceleration contract.
Because the inherited query path still reports WDDM 1.2 while these semantics
are incomplete, registration must remain unreachable. Successful compilation
does not establish that the driver can register, start, render, recover, or
satisfy the mandatory WDDM 1.2 feature contract.

The stable display-only driver remains `viogpudo` and WDDM 1.2.

The focused safety contract for the current slice passes locally:

```text
python viogpu/viogpuwddm/check-contract.py
```

The ARM64 workflows use `windows-2025-vs2026`, conditionally install the pinned
Windows SDK/WDK 28000 packages with the runner's `winget`, verify the required
ARM64 kit files, and emit ARM64 driver targets only. Their x64 tools are
runner-side cross-build and ABI-fixture tools, not product targets. The mutation
suite is intentionally not wired into ordinary or manual CI; do not run it until
the implementation phase is complete and a major contract-boundary validation
is explicitly requested.
