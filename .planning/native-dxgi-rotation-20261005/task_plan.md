# Paired DXVK native DXGI rotation candidate

## Goal
Advance the unregistered DXVK candidate to exact source
4f59adf77cf3d6207c85af85f7a39adff3d8e367 and include its production DXGI
rotation regression in the ARM64 fixture handoff.

## Next step
Follow paired CI 37312228456 for source 5798e077 to completion, then download
and verify its signed package receipts. Root owns source validation and all
guest operations.

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
