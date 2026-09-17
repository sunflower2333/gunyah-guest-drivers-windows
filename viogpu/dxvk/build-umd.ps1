# SPDX-License-Identifier: MIT
# Builds the DXVK D3D10/11 user-mode driver for one architecture from the
# sunflower2333/dxvk commit pinned in viogpu/package/flat_package.py, through
# that tree's own scripts\build-native-umd.ps1. Validates the image (machine,
# exact exports, allowed imports, private Vulkan loader, PDB identity), runs the
# tree's host gates and stages:
#   -Output    the UMD, its PDB, the package load probe and dxvk-umd.json;
#   -Fixtures  the tree's WARP functional fixtures for the ARM64 runner.
# Run inside the target architecture's MSVC developer environment. The UMD
# stays an unregistered candidate: nothing here touches the INF.
param(
    [Parameter(Mandatory = $true)][ValidateSet('arm64')][string]$Architecture,
    [Parameter(Mandatory = $true)][string]$Output,
    [Parameter(Mandatory = $true)][string]$Fixtures
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$out = [IO.Path]::GetFullPath($Output)
$fixtureOut = [IO.Path]::GetFullPath($Fixtures)
foreach ($directory in @($out, $fixtureOut)) {
    if (Test-Path -LiteralPath $directory) { throw "Output directory already exists: $directory" }
    New-Item -ItemType Directory -Path $directory | Out-Null
}
if ($env:VSCMD_ARG_TGT_ARCH -ne $Architecture) {
    throw "MSVC developer environment targets '$env:VSCMD_ARG_TGT_ARCH', not $Architecture"
}

$pin = (& python -c "import sys; sys.path.insert(0, sys.argv[1]); import flat_package; print(flat_package.CANDIDATE_SOURCES['dxvk'])" (Join-Path $repo 'viogpu/package')).Trim()
if ($LASTEXITCODE -or $pin -notmatch '^[0-9a-f]{40}$') { throw 'Cannot read the pinned DXVK commit' }
$parent = (& git -C $repo rev-parse HEAD).Trim()
if ($LASTEXITCODE -or $parent -notmatch '^[0-9a-f]{40}$') { throw 'Cannot read the driver commit' }

$library = @{ arm64 = 'viogpudxvk' }[$Architecture]
$loader = "viogpu_gl_loader_$Architecture.dll"
$machine = @{ arm64 = 'AA64' }[$Architecture]

# Exact source identity: a detached checkout of the pinned commit, never a branch.
$dxvk = Join-Path $env:RUNNER_TEMP "dxvk-umd-$Architecture"
if (Test-Path -LiteralPath $dxvk) { Remove-Item -LiteralPath $dxvk -Recurse -Force }
New-Item -ItemType Directory -Path $dxvk | Out-Null
& git -C $dxvk init --quiet
& git -C $dxvk remote add origin 'https://github.com/sunflower2333/dxvk.git'
& git -C $dxvk fetch --quiet --depth 1 origin $pin
if ($LASTEXITCODE) { throw "Cannot fetch DXVK $pin" }
& git -C $dxvk checkout --quiet --detach FETCH_HEAD
if ($LASTEXITCODE -or (& git -C $dxvk rev-parse HEAD).Trim() -cne $pin) { throw "DXVK checkout is not $pin" }
& git -C $dxvk submodule update --init --recursive --depth 1
if ($LASTEXITCODE) {
    & git -C $dxvk submodule update --init --recursive
    if ($LASTEXITCODE) { throw 'DXVK submodule checkout failed' }
}
Write-Host "DXVK source: sunflower2333/dxvk@$pin"

# The tree's own build entry. Package mode names the per-architecture DLL at
# link time and compiles in the private loader the OpenGL payload ships.
$build = Join-Path $env:RUNNER_TEMP "dxvk-umd-build-$Architecture"
Push-Location $dxvk
try {
    & pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/build-native-umd.ps1 `
        -OutputDirectory $build -LibraryName $library -VulkanLoader $loader
    if ($LASTEXITCODE) { throw "DXVK build-native-umd.ps1 failed for $Architecture" }
} finally {
    Pop-Location
}

$status = Get-Content -LiteralPath (Join-Path $build 'STATUS.txt') -Raw
foreach ($line in @("DXVK_COMMIT=$pin", "ARCH=$Architecture", "LIBRARY=$library.dll",
                    "VULKAN_LOADER=$loader (private, beside the UMD)")) {
    if ($status -notmatch ('(?m)^' + [regex]::Escape($line) + '\r?$')) { throw "DXVK STATUS lacks '$line'" }
}

$dll = Join-Path $build "$library.dll"
$pdb = Join-Path $build "$library.pdb"
$headers = (& dumpbin /nologo /headers $dll) -join "`n"
if ($LASTEXITCODE -or $headers -notmatch "(?im)^\s*$machine machine") { throw "$library.dll is not a $machine image" }

# Exact export table: the runtime entries, the package load check and the
# development helpers the tree documents. Nothing may appear or disappear.
$exportText = (& dumpbin /nologo /exports $dll) -join "`n"
if ($LASTEXITCODE) { throw "Cannot read $library.dll exports" }
$exports = @([regex]::Matches($exportText, '(?m)^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)') |
    ForEach-Object { $_.Groups[1].Value } | Sort-Object)
$expectedExports = @('OpenAdapter10', 'OpenAdapter10_2', 'VioGpuDxvkCreateDdiTestDevice',
    'VioGpuDxvkOpenAdapterForTest', 'VioGpuDxvkPrivateDeviceSize', 'VioGpuDxvkQueryRuntimeAdapterLuid',
    'VioGpuDxvkQueryVulkanLoader') | Sort-Object
if (Compare-Object $expectedExports $exports -CaseSensitive) {
    Write-Host $exportText
    throw "$library.dll export table mismatch: $($exports -join ', ')"
}

# Imports: operating system DLLs only. No CRT DLL (static CRT), no public
# Vulkan loader or D3D runtime linked in; the Vulkan loader is loaded by path.
$importText = (& dumpbin /nologo /dependents $dll) -join "`n"
if ($LASTEXITCODE) { throw "Cannot read $library.dll imports" }
Write-Host $importText
$imports = @([regex]::Matches($importText, '(?im)^\s+(\S+\.dll)\s*$') | ForEach-Object { $_.Groups[1].Value.ToUpperInvariant() })
$allowedImports = @('KERNEL32.DLL', 'USER32.DLL', 'GDI32.DLL', 'ADVAPI32.DLL', 'SETUPAPI.DLL')
$unexpected = @($imports | Where-Object { $_ -notin $allowedImports })
if (!$imports.Count -or $unexpected.Count) { throw "$library.dll imports outside the allowed OS set: $($unexpected -join ', ')" }

# The loader name is compiled in as the UTF-16 path component LoadLibraryExW uses.
$image = [IO.File]::ReadAllBytes($dll)
if ([Text.Encoding]::Unicode.GetString($image).IndexOf($loader, [StringComparison]::Ordinal) -lt 0) {
    throw "$library.dll is not built for its private Vulkan loader $loader"
}
if (!(Test-Path -LiteralPath $pdb -PathType Leaf)) { throw "$library.dll has no PDB" }
& python (Join-Path $repo '.install_scripts/verify-pe-pdb.py') $dll $pdb
if ($LASTEXITCODE) { throw "$library.dll/PDB identity mismatch" }

# Host gates from the pinned tree, against the WDK header the UMD compiled with.
$sdkVersion = ($env:WindowsSDKVersion -replace '\\+$', '')
$wdkHeader = Join-Path ${env:ProgramFiles(x86)} "Windows Kits/10/Include/$sdkVersion/um/d3d10umddi.h"
$loadability = (& python (Join-Path $dxvk 'tests/check-umd-loadability.py') --wdk $wdkHeader) -join "`n"
$loadabilityExit = $LASTEXITCODE
Write-Host $loadability
if ($loadabilityExit -or $loadability -notmatch '(?m)^UMD loadability policy: PASSED\s*$') { throw 'DXVK UMD loadability policy failed' }
$gate = [regex]::Match($loadability, '(?m)^PASS\s+gate-state\s+(.+?)\s*$')
if (!$gate.Success) { throw 'DXVK loadability policy reported no gate state' }
$gateState = $gate.Groups[1].Value
# The package ships DXVK unregistered because its admission gate is closed. An
# open gate is a registration decision for a person, not something CI adopts.
if ($gateState -notmatch '^closed; remaining: ') { throw "DXVK admission gate is not closed: $gateState" }
$integration = (& python (Join-Path $dxvk 'tests/check-wddm-integration.py')) -join "`n"
if ($LASTEXITCODE -or $integration -notmatch 'DXVK WDDM integration policy passed') { throw 'DXVK WDDM integration policy failed' }
Write-Host $integration

# Package load probe, compiled against the same kit's WDK headers.
$probe = "dxvk-umd-probe-$Architecture.exe"
& cl /nologo /W4 /WX /EHsc /MT /external:anglebrackets /external:W0 (Join-Path $repo 'viogpu/dxvk/umd-probe.cpp') `
    "/Fo$(Join-Path $env:RUNNER_TEMP "dxvk-umd-probe-$Architecture.obj")" "/Fe$(Join-Path $out $probe)"
if ($LASTEXITCODE) { throw "Cannot build $probe" }
$probeHeaders = (& dumpbin /nologo /headers (Join-Path $out $probe)) -join "`n"
if ($probeHeaders -notmatch "(?im)^\s*$machine machine") { throw "$probe is not a $machine image" }

Copy-Item -LiteralPath $dll, $pdb -Destination $out
$files = [ordered]@{}
foreach ($entry in @(@("$library.dll", $Architecture, 'candidate-runtime'), @("$library.pdb", 'data', 'symbols'),
                     @($probe, $Architecture, 'probe'))) {
    $files[$entry[0]] = [ordered]@{
        sha256 = (Get-FileHash -LiteralPath (Join-Path $out $entry[0]) -Algorithm SHA256).Hash.ToLowerInvariant()
        machine = $entry[1]
        role = $entry[2]
    }
}
[ordered]@{
    schema = 1
    family = 'dxvk'
    architecture = $Architecture
    activation = 'unregistered-candidate'
    sources = [ordered]@{ dxvk = $pin; parent = $parent }
    vulkan_loader = $loader
    gates = [ordered]@{ loadability = 'PASSED'; gate_state = $gateState; wddm_integration = 'passed' }
    files = $files
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'dxvk-umd.json') -Encoding utf8

# Functional fixtures run natively on the ARM64 runner by the tree's own
# scripts\test-native-arm64.ps1, which also requires STATUS.txt.
$fixtureNames = @('STATUS.txt', "$library.dll", 'dxvk-umd-texture1d-test.exe', 'dxvk-umd-runtime-gpu-test.exe',
    'dxvk-umd-native-entry-test.exe', 'dxvk-umd-native-lifetime-test.exe', 'dxvk-umd-allocation-test.exe',
    'dxvk-umd-predication-test.exe', 'dxvk-umd-stream-output-test.exe', 'dxvk-umd-query-test.exe',
    'dxvk-umd-system-runtime-test.exe')
foreach ($name in $fixtureNames) {
    Copy-Item -LiteralPath (Join-Path $build $name) -Destination $fixtureOut
}
Get-ChildItem -LiteralPath $out -File | Get-FileHash -Algorithm SHA256 | Format-Table -AutoSize | Out-String | Write-Host
Write-Host "PASS DXVK $Architecture UMD $library.dll from $pin; gate $gateState; Vulkan loader $loader; unregistered candidate"
