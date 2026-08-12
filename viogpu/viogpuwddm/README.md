# viogpuwddm compile-only skeleton

This project type-checks a full-miniport-shaped WDDM 1.2 callback table for the
VirtIO GPU 3D path. It is only a compile/link skeleton, not a complete WDDM 1.2
implementation, and is intentionally not installable:

- `DriverEntry` returns `STATUS_NOT_SUPPORTED` without calling the registration
  helper.
- There is no INF, package target, or signing input.
- VirtIO submission fences, preemption, and TDR recovery are not connected.

`VioGpuWddmBuildInitializationData` wires the existing display adapter,
interrupt, power, cursor, EDID, and VidPN lifecycle into a full-miniport
`DRIVER_INITIALIZATION_DATA` table. The separate
`VioGpuWddmInitializeMiniportCompileOnly` helper compiles and links the future
`DxgkInitialize` call, including trace initialization and failure cleanup, but
is deliberately unreachable from `DriverEntry`. The compile-only project sets
`OptimizeReferences=false` so the linker cannot discard that helper before
resolving its WDK contract. On a future successful registration, the registered
`VioGpuDodUnload` callback owns trace cleanup. CI also checks that the linked
`.sys` still imports `DxgkInitialize`.

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

Several callbacks that are registered are still skeletons. Present, submit,
preempt, current-fence query, timeout reset/restart, and VidPN source-address
programming return `STATUS_NOT_SUPPORTED`; paging implements only discard. No
real Host-retirement completion is implemented. Mandatory full-graphics WDDM
1.2 semantics that are absent or unproven include video-memory offer/reclaim,
GPU preemption and FlipOnVSyncMmIo, per-engine TDR, optimized rotation, direct
flip, GDI hardware acceleration, seamless state transitions/PnP, and display
container ID behavior. Because the inherited query path still reports WDDM
1.2 while these semantics are incomplete, registration must remain
unreachable. Successful compilation does not establish that the driver can
register, start, render, recover, or satisfy the mandatory WDDM 1.2 feature
contract.

The stable display-only driver remains `viogpudo` and WDDM 1.2.

Run the focused safety contract with:

```text
python viogpu/viogpuwddm/check-contract.py
```
