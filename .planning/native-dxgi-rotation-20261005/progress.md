# Progress

## 2026-10-05
- Audited workspace safety rules, clean paired checkout and exact source pins.
- Confirmed the new ARM64 rotation fixture must be explicitly copied.
- Created work/native-dxgi-rotation-20261005 from fac1bfe6.
- Root authorized local preparation and commit; push/CI dispatch await full
  DXVK source CI success. No guest or installation operations performed.
- Updated both exact DXVK pins, added ARM64 rotation fixture copying and
  documented the source milestone and older paired baseline.
- Root confirmed full DXVK CI 37310476652 success including native ARM64
  runtime and authorized personal branch push plus direct paired build CI.
- All 73 existing package unit tests and git diff --check passed.
- Committed integration as 5798e077cebbc2e140371cfe71d9327dbbce7865 and
  pushed the personal branch.
- Dispatched direct build-arm64-drivers.yml as CI 37312228456. Full signed
  package validation is in progress; artifacts will be retained under
  artifacts/dxvk-native-rotation-20261005/paired-ci-37312228456/.
- The paired OpenGL job failed only because Mesa CI 34757244563 had expired
  artifacts. Exact-source Mesa rebuild CI 37312838033 completed successfully
  and published three unexpired architecture artifacts. The paired workflow
  now consumes that replacement run ID; source/pin/version remain unchanged.
- Resumed after a service interruption. Root pushed d5e482c4 and dispatched
  replacement paired CI 37318070394; all 14 jobs except product build passed.
- Retained product failure log under paired-ci-37318070394/build-job.log.
  The ARM64 rotation regression passed before the VS18/WDK task failure.
- Audited the established gg-zero-copy VS2022/ARM64 CI split for a narrow
  toolchain repair; no GPU source, installed driver or VM changes.
- Prepared the matching VS2022/x64 cross-build host, separate mandatory
  DXVK native ARM64 fixture job, and separate signed runtime load job.
  Added -StaticOnly for common catalog/signature/installer checks on x64;
  native ARM64/EC/x86 load checks remain mandatory on Windows ARM64.
- All 73 existing package tests and diff check passed after the CI split.
  No local PowerShell parser is installed; Windows CI will parse/execute it.
- Root reviewed the split and confirmed preserved native load/source guards.
  Added always-uploaded ARM64 functional stdout/stderr and executable hashes
  to retain exact test evidence on success or failure.
- Committed/pushed e0449321acc7042b7af9a165ddb366b2f24158e5 with actual
  multiline attribution and dispatched replacement paired CI 37336134109.
  Kept the source pin at 4f59adf pending root's focused Present repair.
- Replacement CI 37336134109 rejected the old literal ARM64 runner assertion
  in check-contract.py. Updated it to require the actual VS2022 cross-build
  and mandatory ARM64 functional/signed-load job split.
- The full miniport contract checker, all 73 package unit tests, YAML parse
  (11 jobs) and git diff --check passed with the stronger per-job runner,
  dependency, toolchain and full-load assertions. Commit/push the checker
  repair now; coordinate the next paired dispatch with root's final DXVK pin.
- Committed/pushed the checker repair as 61d03f05. Root requested cancelling
  superseded CI 37336134109 because its diagnosed checker failure makes the
  product package impossible. Cancellation is not a timeout; its old DXVK
  source has independent validation. Await the reviewed final source pin.
- Confirmed CI 37336134109 completed cancelled and retained run-final.json
  alongside its diagnosed failure log.

## 2026-10-06
- Root reviewed, committed and pushed final DXVK Present ownership source
  34ff484ac87b66f77e1c9f6d8db914783b24788e. Updated both exact package pins
  and current documentation; retained the existing rotation/lifetime fixture
  inventory, all Mesa/VKD3D pins, package version and unregistered admission.
  Source Windows CI is pending. Root authorized paired dispatch concurrently
  after meaningful local full contract/package/YAML/diff validation.
- All 73 existing package tests, workflow YAML parse (11 jobs) and diff check
  passed after the final pin update. The full miniport contract checker is
  running; D3D Mesa gitlink remains eae74a860208698428227d89d05bb63d274a8853.
- Full miniport contract checker completed PASS. Final diff/source-pin review
  confirms unchanged Mesa/VKD3D pins, 100.6.101.58522 and eight closed gaps.
