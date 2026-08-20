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
Escape, and Render identities against a stable reset generation. This still
does not implement the product's guest-backed allocation or submit data path.
`DriverEntry` remains fail-closed and returns `STATUS_NOT_SUPPORTED`; no
artifact from this project may be installed or loaded.

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
range. Before version 1 can be published, the ABI must still define
requested-IOVA bind/unbind/query behavior plus the real guest-backed allocation
and teardown lifecycle. The UMD must query and cache the response before
selecting any BO IOVA or submitting, then discard it when the generation
changes. Escape is not a submit path. Create-time private data is UMD-to-KMD
input and must not be treated as an output channel. D3DKMT allocation and
context handles remain the UMD's opaque identities.

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
`1` and a current nonzero reset-generation token. `Render` bounds command input
to 64 KiB, copies command and patch inputs to nonpaged snapshots, validates their
shared allocation identity, rejects writes through read-only opens and
overlapping 8-byte patch slots, rechecks the reset generation, and only then
publishes its DMA output. UMD-selected patch slot IDs may be nonzero; reserved
patch bits must be zero.

`Patch` deliberately returns `STATUS_NOT_SUPPORTED`. A VidMm segment physical
address is not a Turnip per-context IOVA, and the KMD has no requested-IOVA or
guest-backed BO transport that could provide the real address yet. `SubmitCommand`
likewise remains fail-closed until Host GPU retirement and WDDM fence completion
are implemented. The validation slice must not be described as a working
Render, Patch, or submit pipeline.

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
of the guest BO pool. The compile-only source now implements the blob-zero
control lifecycle only; it is not a BO allocation path. The product path does
not use `NCTX_LEGACY_HOST_ALLOC`, runtime SHARE, pool-outside GPA backing, or a
second offset allocator. Real Windows guest-backed CreateBlob/allocation,
map/unmap, and teardown transport remains a P2 implementation item;
compile/link success here does not provide that behavior.

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
nonpaged/contiguous fallback. This establishes only segment provenance; VidMm
placement, guest-backed blob creation, requested IOVA, and allocation teardown
remain unimplemented.

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
P2 guest-backed BO teardown implementation.

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

- `DxgkDdiNotifyAcpiEvent`, `DxgkDdiControlEtwLogging`, and
  `DxgkDdiCollectDbgInfo`.
- `DxgkDdiSetPalette`, `DxgkDdiGetScanLine`, and `DxgkDdiControlInterrupt`.
- `DxgkDdiQueryDependentEngineGroup`, `DxgkDdiQueryEngineStatus`, and
  `DxgkDdiResetEngine`.
- `DxgkDdiGetChildContainerId`.
- `DxgkDdiSetPowerComponentFState` and
  `DxgkDdiPowerRuntimeControlRequest`.

It also does not register the KMDOD-only `DxgkDdiPresentDisplayOnly` callback.

Present and VidPN source-address programming remain fail-stop skeletons. The
preemption callback validates the single node/engine contract, reports
`DXGK_INTERRUPT_DMA_PREEMPTED` only when the native fence queue is already
empty, and gates the adapter for TDR when work is in flight or the interrupt
notification cannot be synchronized. `PreemptionAware` deliberately remains
zero because Native Context has no Host cancellation/preemption primitive.
`DxgkDdiResetFromTimeout` performs the adapter-wide D3 transport teardown and
advances the reset fence only after teardown succeeds; restart reuses the
checked D0 recovery state machine. Render patch/submit and paging now use
bounded private records; paging transfers consume their MDL during
`DxgkDdiBuildPagingBuffer`, then a cancellable passive worker publishes guest
pool placement and host BO ownership. These paths are compile-only and do not
prove runtime preemption, TDR, or Present behavior.
Mandatory full-graphics WDDM
1.2 semantics that are absent or unproven include video-memory offer/reclaim,
GPU preemption and FlipOnVSyncMmIo, per-engine TDR, optimized rotation, direct
flip, GDI hardware acceleration, seamless state transitions/PnP, and display
container ID behavior. The current `CreateContext` flag check also rejects
system and GDI contexts; it is a narrow UMD-context scaffold rather than a
general WDDM 1.2 context contract. Because the inherited query path still
reports WDDM 1.2 while these semantics are incomplete, registration must remain
unreachable. Successful compilation does not establish that the driver can
register, start, render, recover, or satisfy the mandatory WDDM 1.2 feature
contract.

The stable display-only driver remains `viogpudo` and WDDM 1.2.

Run the focused safety contract with:

```text
python viogpu/viogpuwddm/check-contract.py
```

The mutation suite is intentionally not part of ordinary push or pull-request
CI. Run the ARM64 workflow manually with `run_mutation=true` only at a major
contract boundary.
