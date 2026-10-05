# Paired DXVK native DXGI rotation candidate

## Goal
Advance the unregistered DXVK candidate to exact source
4f59adf77cf3d6207c85af85f7a39adff3d8e367 and include its production DXGI
rotation regression in the ARM64 fixture handoff.

## Next step
Repair the inherited VS18/WDK26100 runner mismatch from paired CI 37318070394.
Use the proven VS2022 cross-build host and retain all native ARM64 functional
and signed runtime load checks in ARM64 jobs. Then validate the replacement
package and receipts. Root owns source validation and all guest operations.

## Phases
- [complete] Audit clean paired baseline and source/fixture requirements.
- [complete] Update exact pins, ARM64 fixture copy and documentation.
- [complete] Commit after successful package tests and diff review.
- [in_progress] Follow paired CI and verify signed package/source receipts.

## Constraints
Preserve reserved package version 100.6.101.58522, all Mesa pins and the eight
admission gaps. Keep DXVK/VKD3D unregistered. No driver installation or remote
operations. This older package must not replace active driver 58623.

## Errors
Audit searches initially used absent .planning and viogpu/docs paths. The
paired checkout has no earlier local plan; source plans are in dxvk-umd-ci
and workspace .planning. No edits resulted from those missing paths.
One documentation cleanup patch included an unrelated context line and failed
atomically. Removed that hunk before applying the intended cleanup.
Paired CI 37312228456 failed because pinned Mesa artifacts expired. Root
refreshed the exact-source run ID to 37312838033 in d5e482c4. Its replacement
paired CI 37318070394 passed every prerequisite and the ARM64 rotation test,
but all driver builds failed before compilation on missing WDK18.0 tasks.
One resume planning patch used the pre-interruption findings text and failed
atomically; reread root's current findings before applying the update.
