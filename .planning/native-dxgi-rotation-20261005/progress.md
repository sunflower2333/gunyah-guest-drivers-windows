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
- Committed/pushed final source integration as
  b6bf4c4f849f5ca597637fc2ce591f23a826f158 and dispatched direct paired CI
  37338579561. Retain run metadata, signed manifest/package verification and
  native/signed-load receipts under workspace
  artifacts/dxvk-native-rotation-20261005/paired-ci-37338579561/.
- Root provided exact 34ff484 source offline CI 37338141939 and full native
  CI 37338144307. Both remain pending; no current source success claim.
- Paired CI cleared the repaired Zink/full-contract job, GL package/native
  ABI, CL package/loader ABI and package tests. Mesa D3D architecture and DXVK
  builds continue without failures.
- Exact 34ff484 DXVK ARM64/x64/x86 paired jobs all passed; their source and
  artifact records are being retained. Mesa x86 passed. Native ARM64 functional
  fixtures are queued while Mesa ARM64/x64 compilation continues.
- Paired native ARM64 functional job 111864053650 passed; retained its output
  and executable hashes artifact 11358520177. Mesa ARM64 also passed, leaving
  x64 Mesa as the final package prerequisite. Installed gh refuses job logs
  until the run completes; retain those logs then rather than retrying now.
- Downloaded input ARM64 fixture artifact 11357009608 and verified STATUS
  exact 34ff484/arm64, lifetime PE AA64 and SHA256
  1a5618ccbb46c6f7c753ab1a0e572140e85242bf1839c9b7a1c359b1e9e88615
  against native CI executable hashes. Native stdout reports lifetime 10834
  checks and rotation 1428 checks/44 locks. Root requested this verified input
  for the remote agent's single guest lifetime run; shared paths/receipt only,
  without guest operations by this agent.
- Downloaded all three DXVK UMD artifacts and retained combined source
  receipts: every architecture records exact 34ff484 and producer b6bf4c4f.
  All 14 prerequisite jobs passed; VS2022 product driver build is running.
  Root reports both changed guest fixtures passed with desktop continuity;
  this candidate remains source 34ff484/version 58522 on the older Mesa/KMD
  baseline, with no installation authorized.
- Verified downloaded three-architecture DXVK source records, every payload
  hash/PE machine, private loaders and exact eight-gap set; retained
  dxvk-source-verified.json. Root confirmed exact source offline CI 37338141939
  all four jobs SUCCESS and full native CI 37338144307 all six SUCCESS,
  including native ARM64. Updated current source evidence; signed claims
  remain pending until product and signed runtime jobs complete.
- Product build 111865802664 passed compilation, PE/exports, signing/package,
  static actual catalog/common signer, exact jointly signed package identity
  and artifact upload. Downloading final droidvm-arm64-drivers; signed Windows
  ARM64 runtime load job remains mandatory and pending.
- Paired CI 37338579561 completed SUCCESS, all 16 jobs, including signed
  Windows ARM64 runtime job 111867949499. Retained final run/artifact metadata,
  product/native/signed-load logs and ci-validation-receipt.json. Both Windows
  catalog checks passed 53 actual members/common signer; installer receipt
  52 files; joint bundle 64 GPU/9 installer files, version 100.6.101.58522.
  Native/EC/x86 GL/CL/D3D and all three DXVK closed-gate/private-loader probes
  passed. Final artifact 11358925550 (138534327 bytes) download remains active
  before full local signed-file hash/source/receipt verification.
- Final download completed without duplicate transfer. Captured original ZIP
  SHA256 1aad40d95c607738fe1e36b64488c7b4ce50e2010e77d3fb1f103db875f8f919
  matches API metadata/upload record. Local verify_bundle used explicit binary
  parent b6bf4c4f, source 34ff484, version 58522 and unchanged exact Mesa/CLVK
  source/run pins: 64 GPU/9 installer hashes and PE/source/gate metadata PASS;
  generated receipt exactly equals uploaded joint-package-receipt.json.
  Retained signed-package-verified.json, candidate-evidence.json and stdout.
- Public cert is CN=DroidVM Test, SHA256
  da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3;
  actual Windows common signer/catalog checks are retained in CI logs.
  Removed duplicate raw ZIP and capture scratch; gh cleaned its temporary
  /tmp archive. Keep extracted signed package and compact evidence/checksums.
  This paired checkpoint is complete; native DX8/DX9 and ordinary candidate
  DX10/DX11 activation/Turnip acceptance remain with root's broader program.
