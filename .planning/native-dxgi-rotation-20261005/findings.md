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
