# Video candidate validation record — 2026-09-15

Candidate: `work/vpu-video-umd-perf-20260915`, independent branch, based on perf
`5ab27d4055887f14e97195079ef51716b7b707fb`.
This is the explicit VioGPU video backend described in README.md, not a
DXVA/D3D Video DDI or registered Media Foundation implementation.

## Observed results

1. At `aebccdc6f495d7b4d0c4f21b90777f4bc9c5bae1`, video workflow
   `34921486799` completed both jobs successfully. It compiled VirtIO/WDF,
   `viogpud3d.dll`, `viogpuvideo.sys`, and `video-codec-test.exe` on an ARM64
   Windows runner with SDK/WDK 10.0.26100.0. Portable ABI/state tests and the
   negative dependency scan also passed. It did not execute device operations.
2. The downloaded build archive SHA-256 is
   `bc224fe9873b655f931793034854adc7ba01774ff0e85dd77a54f53b639c960c`.
   Local PE inspection found machine 0xaa64 for SYS/DLL/EXE. The SYS imports
   only ntoskrnl.exe, HAL.dll and WDFLDR.SYS. The DLL retains the three legacy
   OpenAdapter exports and adds exactly the eight documented VioGpuVideo
   operations. This archive is an unsigned review build, not a signed release.
3. The first media builds exposed and then fixed INF error 1199 (DIRID 13
   requires an appropriate minimum OS decoration) and user/kernel CRT header
   conflicts. Warning-as-error and INF validation remained enabled.
4. At review run `34922032823`, the new video files were formatted with the
   repository's clang-format 16. Production ABI/state tests, the no-pool test,
   and the full WDDM contract passed. Negative controls removed OpenAdapter,
   removed VioGpuVideoQueue, and inserted OpenAdapter12; the real checker
   rejected each mutation. The output commit is
   `ca9d38d097ab484b43c0aff46bc68b6ccab925bc`.
5. The production checker keeps its retired-pool scan unchanged. Only its
   exact export list (three legacy plus eight explicit video functions) and
   exact source list (activation source plus video_client.cpp) were extended
   in that initial delivery. Source comments use generic ordinary-RAM wording
   because the production scan intentionally also rejects comment mentions;
   the explanatory details remain in README.md and dependency tests.

The temporary review workflows and helpers are removed from the final tree.
The independent branch's build/test workflows retain read-only repository
permissions.

## Not yet validated

No real Windows media-device enumeration, driver loading, MediaCodec session,
frame quality/count, tail drain, DRC, process-exit/reset stress, or throughput
measurement was executed in this session. Builds do not establish any of those
results. The hardware acceptance program and its input restrictions are in
README.md. The current host still requires its media_host allocation; no
Windows restricted-pool service is introduced.

Existing repository signoff/Jira checks were failing at the initial PR
revision and no fake signoff was added. PR #3 was closed without merging at
the owner's request. No open PR is required. Standard API integration and
hardware acceptance remain separate from branch CI.

## Independent branch CI follow-up

PR #3 is closed and the perf branch is not updated by this work. Direct pushes
to `work/vpu-video-umd-perf-20260915` now trigger the dedicated media build,
full Native Context WDDM build, performance regressions and the existing
repository-wide format checks. PR-only signoff/Jira checks were not disabled
or fabricated; they are not part of the branch-push verification.

Both production binary-export gates retain the three legacy entry points
and require exactly eight explicit video functions, rejecting missing, extra
and duplicate exports. The new PowerShell tests execute those actual gates,
including against the built UMD. Existing INF, ARM64 PE, map and ownership
checks remain in place.

Formatting changes use the existing repository clang-format 16 settings.
Source-contract normalization accounts for split wide identifier strings
without discarding identifier spelling, operators or counter indices. The
Advanced Color mutation tests use unique whitespace-insensitive anchors and
retain all 33 negative controls. No tests were deleted to obtain a green run.

Consult the exact final commit's Actions checks for its build results; earlier
artifacts above are historical and must not be mistaken for new binaries.
Build success remains distinct from driver loading or actual codec operation.
