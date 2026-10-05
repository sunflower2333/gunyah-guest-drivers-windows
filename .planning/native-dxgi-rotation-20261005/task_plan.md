# Paired DXVK native DXGI rotation candidate

## Goal
Advance the unregistered DXVK candidate to exact source
4f59adf77cf3d6207c85af85f7a39adff3d8e367 and include its production DXGI
rotation regression in the ARM64 fixture handoff.

## Next step
Commit the verified integration, then push the personal branch and dispatch
build-arm64-drivers.yml. Root confirmed full DXVK CI 37310476652
success and authorized these steps. Root owns source validation and all
guest operations.

## Phases
- [complete] Audit clean paired baseline and source/fixture requirements.
- [complete] Update exact pins, ARM64 fixture copy and documentation.
- [in_progress] Commit after successful package tests and diff review.
- [pending] Push and run paired CI after confirmed full source CI success.

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
