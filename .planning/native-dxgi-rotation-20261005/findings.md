# Findings

Baseline fac1bfe6dbb79ad6ebd3ea4ee2e1476f170b1e7c pins DXVK a677ab0 in
flat_package.py and build_candidate_umds.ps1. DXVK 4f59adf implements
pfnRotateResourceIdentities; DXGI coverage advances to 2/7 while the exact
eight admission gaps remain.

The ARM64 build wrapper copies a fixed fixture inventory. The new source's
test-native-arm64.ps1 requires dxvk-umd-rotation-test.exe, so both pin updates
also require adding it to that inventory. x64/x86 execute it through the
source's build-native-umd.ps1.

The paired package reserves 100.6.101.58522 atop frozen KMD 58473. D3D Mesa is
eae74a860208698428227d89d05bb63d274a8853; GL/Turnip Mesa is
3e50dd4ba4f941fcb4ddeb91d0cbd688cabfdb4a from CI 34757244563. Installed 58623
uses newer Mesa b36366b53c17cc55c84df7b756b45fb6e4e4e8fa. Treat this build
as candidate integration evidence, without changing the installed stack.

build-arm64-drivers.yml is the direct build/sign/catalog/load workflow.
dev-release.yml also publishes the rolling release and is unnecessary here.

Paired CI 37312228456 correctly fetched the pinned D3D Mesa source, but its
OpenGL package job failed because the successful source run 34757244563 had
expired all artifacts. A fresh exact-source Mesa rebuild, run 37312838033,
completed successfully with three unexpired architecture artifacts. The
paired workflow now consumes that run ID; source commit and package version
remain unchanged.

Paired CI 37318070394 passed all prerequisites and executed native ARM64
rotation: 1281 checks/43 locks. Its build job failed before source compilation
because windows-11-arm has VS18/MSBuild18.10 with WDK26100 tasks compiled only
for17.0. The existing gg-zero-copy workflow already resolves this pairing by
cross-building on windows-2022 with x64 MSBuild and running signed ARM64X
loads in a separate Windows ARM64 job. Adapt that proven split without
renaming task DLLs or changing the kit/source/version pins.

The first replacement split CI, 37336134109, failed the inherited literal
runner assertion in check-contract.py. The strengthened checker now enforces
the product VS17/x64 cross-build host, mandatory native ARM64 DXVK fixture
dependency, and mandatory signed ARM64 runtime-load job without -StaticOnly.
The full contract checker and 73 package tests pass. Root requested cancelling
the superseded run after this diagnosed failure, ahead of the final Present
source integration. Two exploratory searches used absent package/workflow
paths; corrected the search to existing viogpu/package and workflow files.
The retained artifacts root is /home/sunf/droidvm-repos/artifacts, not a path
relative to gg-dxvk-umd. Corrected one attempted metadata redirection before
successfully saving the cancelled run's final state there.

Root reviewed and pushed final DXVK source
34ff484ac87b66f77e1c9f6d8db914783b24788e. It retains the runtime DXGI callback
table, separately pins Present allocation/readback/backend owners, and uses a
live registry reservation to cancel submission after source retirement.
Device-wide Present exclusion and mutual rotation exclusion protect shared
publication and staging maps. The source fixtures poison private storage
immediately from LockCb/CreateContextCb/PresentCb and check balanced deferred
release and rejected nested operations. Fixture names and eight admission
gaps are unchanged. Local source ASan/UBSan 69/180/147 and 101-slot WDK policy
passed; exact Windows source CI remains pending. Root authorized concurrent
paired CI after the local package checks.

Exact 34ff484 source offline CI 37338141939 and full native CI 37338144307
passed all four/six jobs. Paired CI 37338579561, producer b6bf4c4f, passed all
16 jobs after the VS17/WDK repair. Windows x64 static and Windows ARM64 full
checks both verified 53 actual catalog members/common signer and the 52-file
manifest; the final joint receipt lists 64 GPU/9 installer files. Runtime
checks loaded all native/EC/x86 GL/CL/D3D/DXVK paths. This validates packaging
and closed-gate loadability only; native DX8/DX9, ordinary DX10/DX11 candidate
runtime activation, Turnip hardware rendering and installation remain open.
