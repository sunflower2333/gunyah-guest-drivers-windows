# Paired DXVK native DXGI rotation candidate

## Goal
Advance the unregistered DXVK candidate to exact source
34ff484ac87b66f77e1c9f6d8db914783b24788e with runtime callback-table lifetime,
Present ownership across callback retirement, and nested Present/rotation
exclusion. Keep the production DXGI rotation/lifetime ARM64 fixture handoff.

## Next step
Paired CI 37338579561 for binary producer b6bf4c4f (DXVK 34ff484) passed all
16 jobs; final archive, signed manifest, source pins, file hashes and Windows
catalog/load receipts are verified. Record the documentation checkpoint.
Root owns broader native runtime capability work and all guest operations.

## Phases
- [complete] Audit clean paired baseline and source/fixture requirements.
- [complete] Update exact pins, ARM64 fixture copy and documentation.
- [complete] Validate the final Present ownership source pin locally.
- [complete] Commit and dispatch the final source integration.
- [complete] Follow paired CI and verify signed package/source receipts.

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
Replacement CI 37336134109 caught the old checker requiring exactly one
windows-11-arm product runner. Updated the contract to enforce the VS2022
cross-build job and both mandatory native ARM64 validation jobs.
The final-pin commit initially staged its temporary message file through a
directory add. Removed that disposable file and amended the unpushed commit;
future commits stage planning files explicitly.
Installed gh lacks run watch --compact. Use its supported --interval and
--exit-status flags; the unsupported flag did not affect the dispatched run.
