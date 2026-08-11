# viogpuwddm compile target

This project type-checks the full WDDM 1.2 miniport contract for the
VirtIO GPU 3D path. It is intentionally not installable:

- `DriverEntry` returns `STATUS_NOT_SUPPORTED` before `DxgkInitialize`.
- There is no INF, package target, or signing input.
- VirtIO submission fences, preemption, and TDR recovery are not connected.

The stable display-only driver remains `viogpudo` and WDDM 1.2.