# viogpuwddm compile-only skeleton

This project type-checks a full-miniport-shaped WDDM 1.2 callback table for the
VirtIO GPU 3D path. It is only a compile/link skeleton, not a complete WDDM 1.2
implementation, and is intentionally not installable:

- `DriverEntry` returns `STATUS_NOT_SUPPORTED` without calling the registration
  helper.
- There is no INF, package target, or signing input.
- VirtIO submission fences, preemption, and TDR recovery are not connected.

## Native Context scope

The current artifact extends the P1 compile-only transport-readiness slice with
a P2 pre-v1 private-ABI validation scaffold. Its unreachable source path
negotiates the required VirtIO-GPU features, validates the Native Context
capset, exposes an exact-revision WDK-independent UMD/KMD snapshot, and
validates allocation, context, and Render identities against a stable reset
generation. This still does not implement the product's guest-backed allocation
or submit data path.
`DriverEntry` remains fail-closed and returns `STATUS_NOT_SUPPORTED`; no
artifact from this project may be installed or loaded.

The fixture in `viogpu/tests/wddm-private-abi/` locks only the current pre-v1
snapshot for independent KMD and UMD endpoint regression checks. It does not
freeze a version-1 ABI. Before version 1 can be published, the contract must add
a low-frequency, context-scoped `D3DKMTEscape(DRIVERPRIVATE)`
`GET_CONTEXT_INFO` request that returns the real Host-created `VaStart`,
`VaSize`, and `ResetGeneration` under context rundown protection. The UMD must
query and cache that response before selecting any BO IOVA or submitting, then
discard it when the generation changes. Escape is not a submit path. The ABI
must also define requested-IOVA bind/unbind/query behavior for an allocation and
the real guest-backed allocation and teardown lifecycle. Create-time private
data is UMD-to-KMD input and must not be treated as an output channel. D3DKMT
allocation and context handles remain the UMD's opaque identities; KMD or
transport object IDs are not exposed.

The current snapshot contains no Windows pointers or handles, physical or IOVA
addresses, or VirtIO/KGSL object identifiers. `CreateAllocation` retains the
validated private data and `OpenAllocation` requires an exact byte-for-byte
match. Every open wrapper holds its owning device through `CloseAllocation`,
records read-only access, and is rejected if a Render context from another
device references it. Render ranges are bounded by the logical UMD-declared
allocation size, not page-aligned VidMm backing padding. Open/close and destroy
also reject unsupported flags, resource-private data, and duplicate handle
arrays before releasing objects. `CreateContext` accepts only the single-engine
affinity mask `1` and a current nonzero reset-generation token. `Render` bounds
command input to 64 KiB, copies command and patch inputs to nonpaged snapshots,
validates their shared allocation identity, rejects writes through read-only
opens and overlapping 8-byte patch slots, rechecks the reset generation, and
only then publishes its DMA output. UMD-selected patch slot IDs may be nonzero;
reserved patch bits must be zero.

`Patch` deliberately returns `STATUS_NOT_SUPPORTED`. A VidMm segment physical
address is not a Turnip per-context IOVA, and the KMD has no context-info/BO
transport that could provide the real address yet. `SubmitCommand` likewise
remains fail-closed until Host GPU retirement and WDDM fence completion are
implemented. The validation slice must not be described as a working Render,
Patch, or submit pipeline.

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
of the guest BO pool. The product path does not use `NCTX_LEGACY_HOST_ALLOC`,
runtime SHARE, pool-outside GPA backing, or a second offset allocator. Real
Windows CreateBlob/allocation, map/unmap, and teardown transport remains a P2
implementation item; compile/link success here does not provide that behavior.

The compile-only target now discovers the exact `drm2kgsl_host` provider and
uses its versioned direct interface. The discovery IOCTL exposes only name,
GPA, and size. Provider and client rundown protection make the kernel VA valid
only inside a successful `AcquireMapping`/`ReleaseMapping` interval, and
provider `ReleaseHardware` waits for those intervals before unmapping. This is
not yet a complete cross-stack PnP contract: viogpu must register for target
device change notification, release the remote interface on query-remove or
surprise-remove, and reconnect after remove-cancelled before any blob data path
can be enabled. A retained file or direct-interface reference is not a mapping
lease and must not be treated as removal coordination.

The current initialization transport tears down fail-closed. It releases every
tracked control/display suballocation before issuing the RDMA arena FREE, clears
the connection only after the provider confirms success, and retains the
adapter plus cached failure after an error or timeout. It never retries an
outcome-uncertain FREE. This avoids freeing potentially device-owned DMA, but it
is not a complete removal contract: `DxgkDdiRemoveDevice` is required to free
the miniport context, while the current failure path must retain that context
because the inner adapter can still reference it. This conflict remains an
explicit registration blocker, alongside the missing P2 guest-backed BO
teardown implementation.

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
- `DxgkDdiCancelCommand` and `DxgkDdiGetChildContainerId`.
- `DxgkDdiSetPowerComponentFState` and
  `DxgkDdiPowerRuntimeControlRequest`.

It also does not register the KMDOD-only `DxgkDdiPresentDisplayOnly` callback.

Several callbacks that are registered are still skeletons. Patch, Present,
submit, preempt, current-fence query, timeout reset/restart, and VidPN
source-address programming return `STATUS_NOT_SUPPORTED`; paging implements
only discard. No real Host-retirement completion is implemented. Mandatory full-graphics WDDM
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
