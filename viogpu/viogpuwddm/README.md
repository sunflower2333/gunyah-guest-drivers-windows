# viogpuwddm Native Context full miniport

This project implements an ARM64 Native Context full miniport for the crosvm
VirtIO GPU path. The project keeps the Win8 WDK declaration surface required by
existing internal source, but dxgkrnl sees a
`DXGKDDI_INTERFACE_VERSION_WIN7` registration table matching the reported
legacy `DXGKDDI_WDDMv1` profile. It does not claim WDDM 1.2 capabilities: the
mandatory WDDM 1.2 preemption, per-engine reset/TDR, direct-flip, and related
contracts remain disabled. The source contains the registration entry point, a
dedicated display-class INX, and an ARM64-only fail-closed D3D UMD shim.
The last committed activation batch passed the ARM64 compile, link, MAP, INF,
signing, and package gates in the dedicated and product workflows. Signed
package `100.6.101.58015` was installed in the unprotected Windows VM, starts
the adapter, and reaches the CDD `DxgkDdiPresent` path. The display remains
black because the observed Present is rejected before Patch or Submit. The
current pageable-GDI correction passes the focused local contract, ABI,
formatting, and syntax gates but has not passed a new ARM64 run or guarded
device test. Successful 2D display, KMT/Host/GPU execution, TDR, and uninstall
rollback remain unverified:

- `DriverEntry` calls the single `DxgkInitialize` registration helper.
- `viogpuwddm.inx` binds ARM64 Windows 11 guests to
  `PCI\VEN_1AF4&DEV_1050`; the signed product workflow stages it separately from
  the stable display-only fallback. Both the focused and signed-product ARM64
  workflows run `InfVerif /w /v`, verify the SYS and `viogpud3d.dll` PE
  machines as `AA64`, and require exactly `OpenAdapter`, `OpenAdapter10`,
  and `OpenAdapter10_2` from the D3D shim.
- The INX removes a stale `RequireRestrictedDma` device setting left by older
  protected-VM packages. Merely omitting the setting does not delete it during
  an upgrade; the current unprotected path must not let dxgkrnl retain that
  retired adapter contract.
- Native ring-1 fence retirement and reset-generation/TDR source contracts are
  wired. Hardware preemption, per-engine reset, smooth rotation, and driver
  color conversion are explicitly not advertised.

The current Windows runtime evidence is narrower than those source contracts.
A unique ETL capture from package `100.6.101.58015` contains two `DdiPresent`
calls, four paging calls, and no `DdiRender`, `DdiPatch`, or
`DdiSubmitCommand` call. Both Present calls return `STATUS_NOT_SUPPORTED`;
dxgkrnl reports both `Driver failed Present` and `PresentFromCdd ... failed`.
The first-failure breadcrumb classifies the rejection as
`GdiSourcePlacement`: the source allocation is a valid context-zero,
CPU-visible GDI allocation with the same 1280x1024 format and pitch as the
primary, but its allocation-list entry has `SegmentId=0`. It consequently has
no aperture MDL/address or 2D Host backing. The package's
`PermanentSysMem` flag prevents VidMm from paging that source into the aperture,
while Present Build incorrectly requires residency before it has returned the
patch records that let VidSch make the source resident.

The current source leaves CPU-visible allocations pageable, separates static
GDI allocation identity from live aperture/Host identity, and defers placement
and backing validation for a `SegmentId=0` allocation until Patch. A nonzero
prepatched source is still required to have a complete current mapping and 2D
backing; Patch and Execute retain those checks unconditionally. Reset
reconciliation recreates backing from the current VidMm PFNs. Native allocation
context, generation, residency, blob, and Host ownership checks are unchanged.
The first classified Present rejection is persisted as a reason/status plus
allocation, placement, format, rectangle, and allocation-list snapshot;
`NativePresentReason` is the commit marker. The read-only decoder is
`.install_scripts/viogpu-native-present-diagnostics.ps1`. None of this is device
success evidence until a newly built package reaches Patch, Submit, a completed
scheduler fence, Host transfer/flush, and visible pixel change.

Do not install an artifact produced from the activation work until guarded VM
validation has passed. The current CI artifacts prove source/build/package
gates only; they do not prove registration, loading, GPU execution, Present,
TDR, or rollback behavior.

The matching Mesa branch builds `vulkan_freedreno.dll` and an independent
`turnip-wddm-icd.ps1` manager. The KMD package does not copy or register the
Vulkan ICD: kernel-driver rollback and Vulkan-loader rollback remain separate,
and neither package silently replaces the stable `viogpudo` files.
`viogpud3d.dll` is not Turnip and is not a rendering UMD: its three legacy
entry points return `E_NOTIMPL` solely to provide a deterministic loader and
revision boundary. It deliberately has no `OpenAdapter12` export.

## Native Context scope

The current implementation extends the P1 transport-readiness work with a P2
pre-v1 private ABI. Its registered source path
negotiates the required VirtIO-GPU features, validates the Native Context
capset, exposes an exact-revision WDK-independent UMD/KMD snapshot, creates a
Host Native Context and its blob-0 control resource, obtains the context VA
range through exact `GET_PARAM` requests, and validates allocation, context,
Escape, and Render identities against a stable reset generation. The source
path also covers guest-backed VidMm placement and Host blob ownership,
Patch/SubmitCommand, ring-1 fence publication and retirement, and retry-safe
allocation teardown. Host GPU execution, Windows loading, runtime fence
retirement, Present, and aperture behavior remain unverified after activation.

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
Host3D blob with blob id zero, assigns a page-aligned slot in the standard
VirtIO GPU host-visible PCI shared-memory region, and maps the resource into
that slot with `RESOURCE_MAP_BLOB`. The driver accepts only the standard cached
map response, seeds the bounded response slot before the synchronous VirtIO
submit, and consumes an exact sequence and response layout afterwards.
Malformed BAR bounds, responses, faults, or VA ranges poison the transport.
Only after both values
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
through the ring-1 fence path, but Host GPU retirement, Windows loading, and
WDDM runtime behavior are still unverified. The source must not be described as
a working driver before those gates pass.

Turnip command streams and shader-visible buffer addresses contain raw IOVAs,
so the legacy MSM submit packet cannot derive a complete per-command resource
closure. The UMD therefore includes every live WDDM BO in each nonempty submit,
and VidMm keeps that complete allocation list resident until retirement. The
shared UMD/KMD list limit is 1024; the 1024 BO records, private references, and
up to 256 command records fit in the 64 KiB DMA buffer. Both temporary KMD
tables and the submission-owned reference tables are allocated to the actual
reference count instead of using large kernel-stack or fixed per-submission
arrays. The UMD prevents creation of a 1025th live BO and returns
`VK_ERROR_OUT_OF_DEVICE_MEMORY`, so a legal live set cannot overflow the submit
contract. This is a bounded correctness baseline, not exact dependency
tracking or a scalability result.

The product baseline is an unprotected VM with `udmabuf=true`. The product
transport must fail closed unless
`VIRTIO_GPU_F_VIRGL`, `VIRTIO_GPU_F_CREATE_GUEST_HANDLE` (feature bit 6),
`VIRTIO_GPU_F_RESOURCE_BLOB`, and `VIRTIO_GPU_F_CONTEXT_INIT` are offered and
acknowledged. GPU retirement uses the control-header fence and ring semantics;
there is no additional generic fence feature bit. For a guest-backed BO, the
KMD/UMD contract is:

- let VidMm supply ordinary guest RAM through `MAP_APERTURE_SEGMENT` and set
  `MSM_BO_GUEST_ALLOC`;
- issue `RESOURCE_CREATE_BLOB` with `VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST` and
  `VIRTIO_GPU_BLOB_FLAG_CREATE_GUEST_HANDLE`;
- derive page-aligned `GPU_MEM_ENTRY` records from the VidMm-owned MDL PFNs,
  coalescing physically adjacent pages, and provide the complete SG table so
  crosvm can build the bounded udmabuf imported by drm2kgsl.

The VidMm segment is a 4 GiB non-CPU-visible aperture address space; its offsets
are placements, not host physical addresses. CPU-lockable non-primary
allocations request permanent system-memory backing. The miniport copies the
supplied PFNs into driver-owned metadata, accepts partial map sequences, and
creates Host backing only after every allocation page is present. Its synthetic
MDL is only a KMD mapping description for the already locked VidMm pages; it
does not allocate or replace those pages. Before a partial unmap changes any
PFN, the miniport retires the Host resource, releases that internal CPU mapping,
and marks the affected pages with the `DummyPage` PFN. A later complete map
recreates Host backing from the current PFN set.

Blob zero is independent of guest BO backing. The driver discovers VirtIO PCI
shared-memory capability type 8, region id 1, validates its 64-bit offset and
size against the referenced BAR, maps that BAR cacheable, and allocates a 16 KiB
slot per live Native Context. The response helpers use only the assigned mapped
slot and do not use an ACPI resource or auxiliary driver interface.

The Native Context target still uses ordinary contiguous/nonpaged fallback
storage for VirtIO bookkeeping and does not acknowledge
`VIRTIO_F_ACCESS_PLATFORM`. Unknown CreateBlob/UNREF results poison the
generation rather than guessing ownership. Runtime crosvm map/unmap behavior,
udmabuf import, cache coherence, and teardown remain unverified; compile/link
success does not prove those behaviors.

`VioGpuWddmBuildInitializationData` wires the existing display adapter,
interrupt, power, cursor, EDID, and VidPN lifecycle into a full-miniport
`DRIVER_INITIALIZATION_DATA` table. `DriverEntry` calls the C-linkage
`VioGpuWddmInitializeMiniport` helper, which initializes tracing before
`DxgkInitialize` and unwinds tracing if registration fails. After successful
registration, `VioGpuDodUnload` owns trace cleanup. CI checks that the final linker map retains the helper
from `driver_entry.obj` and resolves
`DxgkInitialize` from `displib:displib.obj`. `DxgkInitialize` is not expected to
remain in the linked driver's PE import table because `displib.lib` supplies a
static implementation; the stable display-only driver has the same behavior.

The inherited display callback implementation and the full-miniport entry point
share one WPP provider. `driver_entry.cpp` is the sole WPP initialization owner;
the project-local `wpp-non-owner.tpl` lets the reused `driver.cpp` compile its
trace calls and cleanup reference without emitting a second provider definition.

The legacy runtime table registers the full-graphics slots that the previous
ETW trace reported missing: ACPI notification, ETW control, palette, scanline,
interrupt control, and `RenderKm`. Unsupported operations fail closed and
`RenderKm` never sends CDD commands through the MSM parser. The Win8-only
`CancelCommand`, per-engine TDR, post-display-ownership, and system-display
callbacks remain implemented internally where needed by existing source but
are not exposed through the Win7 registration table. The KMDOD-only
`DxgkDdiPresentDisplayOnly` callback is also not registered.

`DxgkDdiCollectDbgInfo` remains registered in the legacy table and emits one
bounded 32-byte snapshot using atomic reset and fence queries. Adapter-wide
timeout recovery remains available. `CancelCommandAware` is zero because the
legacy table has no cancellation callback. `SupportPerEngineTDR` is beyond the
Win7 DriverCaps prefix and is not accessed or advertised.

Standard paging now covers primary, GDI shadow, and staging allocations. It
uses context-zero paging records, copies or fills the VidMm-backed aperture
mapping, keeps each allocation's low-range local resource ID stable, and
recreates primary 2D Host backing after a confirmed reset epoch.
CPU-visible sources remain cached but are not marked `PermanentSysMem`, so
VidSch can page them into that aperture before Patch.
The aperture is reported CPU-visible and cache-coherent because shared
primaries can only advertise writable segments with those properties; its
backing remains ordinary guest RAM supplied through `MapApertureSegment`.
`DxgkDdiSetVidPnSourceAddress` validates and selects only a resident standard
primary with current 2D backing.
When a VidPN source becomes invisible, the driver confirms an all-zero
`SET_SCANOUT` before publishing the hidden state. Aperture unmap and allocation
destroy use a mutex-serialized detach-if-current operation, so retirement of an
old primary cannot disable a newer scanout. Host ownership is unreferenced only
after that detach is confirmed, and a failed unmap keeps the original PFNs and
requests reset while returning a paging retry status instead of an UNMAP-invalid
allocation-busy status.

The source also implements a synchronous CPU-copy `DxgkDdiPresent` path. A
Native context may present only an exact live native allocation identity; a GDI
context may identify only a CPU-visible non-primary standard allocation whose
context and reset identity fields are zero. Build validates bounded rectangles,
allocation opens, and patch records. It validates current placement and Host
backing immediately only for an allocation whose input list has a nonzero
`SegmentId`; `SegmentId=0` explicitly defers those checks to Patch. Patch and
Execute require the GDI source and standard primary to be fully resident with
current backing attached to the same VidMm PFNs. Build always returns both patch
records, and Submit may skip Patch only when both source and destination were
prepatched.
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
a passive transaction that copies the selected rows in the VidMm-backed
aperture mapping, issues the 2D transfer/flush, and reports the scheduler fence.
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
The pageable-GDI correction passes the local contract, native-context wire ABI,
WDDM private ABI, Python compilation, clang-format, and diff gates. It has not
yet passed the ARM64 compile/link/package workflow or device runtime. Package
`100.6.101.58015` predates this correction and proves that the earlier
live-identity change alone did not make 2D Present work.

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
then verifies that the mapped allocation and its Host backing remain current.
Paging cancellation signals every recognized transaction even after the passive
worker owns the batch. A structurally valid private range publishes its record
count before deep record validation so earlier recognizable owners can release
allocation references if a later record is stale or malformed.

`DXGKARG_SUBMITCOMMAND` does not expose a CPU DMA-buffer base. Render and
Present Submit therefore validate submission offsets and sizes against the
KMD-private owner's retained DMA pointer; paging resolves its retained packet
pointer directly. Paging Submit interprets the first union member as `hDevice`,
including zero-length system packets issued while a device is being destroyed.
Patch and Cancel still use their supplied CPU DMA base for an exact
pointer-plus-offset identity check.

Paging packets are scheduler system commands and are never reported through
`DXGK_INTERRUPT_DMA_FAULTED`. An empty paging packet is recorded and retired
under one fence-tracker lock before the hardware-operation gate. A queued
paging failure retires the already recorded system fence and requests an
adapter reset; validation, operation-gate, queue, and cancellation failures use
the same complete-then-reset policy. Render and Present retain the ordinary
client-submission fault path.

Submission-fault recovery no longer trusts the callback identity as a
prerequisite for reset. A zero fence or nonzero node/engine identity first
closes the outer hardware epoch, invalidates the pending native fence tracker,
and fails the inner Native Context transport. Only the scheduler's
`DXGK_INTERRUPT_DMA_FAULTED` notification is suppressed when its identity
cannot be represented legally.

The current source deliberately leaves three runtime assumptions unproven. A
Built paging record already owns the allocation, but still relies on VidMm
retaining the DMA buffer and KMD private record until Patch, SubmitCommand, or
CancelCommand. Each Present multipass retry is assumed to retain every
previously published private record until its corresponding submission retires.
CPU row copies and paging copies also assume cache coherence between the
Normal-WB VidMm mapping and Host KGSL access; memory barriers order CPU accesses
but do not add a platform CPU/GPU cache-maintenance primitive.
These are explicit runtime gates; source inspection does not prove TDR, paging,
or Present behavior. Optional capabilities that are not implemented remain
disabled, including hardware preemption, FlipOnVSyncMmIo, per-engine TDR,
rotation, direct flip, and GDI acceleration beyond the CPU-copy baseline.
System contexts provide typed lifecycle ownership and GDI contexts support the
bounded Present baseline. Successful compilation alone still does not establish
that the driver can register, start, render, recover, or satisfy its advertised
legacy WDDMv1 profile in a Windows runtime.

The stable display-only driver remains `viogpudo` and WDDM 1.2.

The focused safety contract for the current slice passes locally:

```text
python viogpu/viogpuwddm/check-contract.py
```

The ARM64 workflows use `windows-2022`, locate and verify a complete preinstalled
Windows SDK/WDK with the required ARM64 kit files, and emit ARM64 driver targets
only. Their x64 tools are
runner-side cross-build and ABI-fixture tools, not product targets. The mutation
suite is intentionally not wired into ordinary or manual CI; do not run it until
the implementation phase is complete and a major contract-boundary validation
is explicitly requested.
