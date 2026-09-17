# SPDX-License-Identifier: MIT
# Builds the ARM64X D3D UMD entry (viogpud3dx.dll) and one load probe per
# process architecture. The Mesa UMDs themselves come from the mesa-umd jobs.
param([string]$Output = 'd3d10-front')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$source = (Resolve-Path (Join-Path $PSScriptRoot '.')).Path
$out = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Force $out | Out-Null
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC installation not found' }
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
function Invoke-Compiler([string]$arch, [string[]]$commands) {
    $script = Join-Path $out "build-$arch.cmd"
    @('@echo off', "call `"$dev`" -no_logo -host_arch=x64 -arch=$arch", 'if errorlevel 1 exit /b 1') +
        @($commands | ForEach-Object { $_; 'if errorlevel 1 exit /b 1' }) |
        Set-Content $script -Encoding ascii
    & cmd.exe /d /c $script
    if ($LASTEXITCODE) { throw "MSVC $arch build failed" }
}
$exports = @('OpenAdapter10', 'OpenAdapter10_2', 'VioGpuD3DUmdTarget')
foreach ($view in @('arm64', 'x64')) {
    @('EXPORTS') + $exports | Set-Content (Join-Path $out "front-$view.def") -Encoding ascii
}
foreach ($arch in @('arm64', 'x64', 'x86')) {
    Invoke-Compiler $arch @(
        "cl /nologo /W4 /WX /EHsc /MT `"$source/umd-probe.cpp`" /Fo`"$out/probe-$arch.obj`" /Fe:`"$out/d3d-umd-probe-$arch.exe`""
    )
}
# Native view first, as its own DLL, only to capture the exact native link
# inputs (object plus static CRT libraries) for the ARM64X merge.
Invoke-Compiler arm64 @(
    "cl /nologo /W4 /WX /EHsc /MT /LD `"$source/umd-front.cpp`" /Fo`"$out/front-arm64.obj`" /link /DEF:`"$out/front-arm64.def`" /OUT:`"$out/viogpud3dx_arm64.dll`" /PDB:`"$out/viogpud3dx_arm64.pdb`" /DEBUG /LINKREPROFULLPATHRSP:`"$out/arm64-inputs.rsp`""
)
$nativeInputs = @(Get-Content "$out/arm64-inputs.rsp" | Where-Object { $_ -match '(?i)\.(obj|lib)"$' })
if (!$nativeInputs.Count) { throw 'No native ARM64 link inputs captured' }
$nativeInputs | Set-Content "$out/arm64-merge.rsp" -Encoding ascii
Invoke-Compiler arm64 @(
    "cl /nologo /W4 /WX /EHsc /MT /c /arm64EC `"$source/umd-front.cpp`" /Fo`"$out/front-arm64ec.obj`"",
    "link /DLL /MACHINE:ARM64X `"$out/front-arm64ec.obj`" @`"$out/arm64-merge.rsp`" /DEFARM64NATIVE:`"$out/front-arm64.def`" /DEF:`"$out/front-x64.def`" /OUT:`"$out/viogpud3dx.dll`" /PDB:`"$out/viogpud3dx.pdb`" /DEBUG"
)
# Prove both views are present before anything is packaged: an ARM64X image
# carries the hybrid metadata sections, and both export tables name the entries.
$headers = (& dumpbin /headers "$out/viogpud3dx.dll") -join "`n"
if ($LASTEXITCODE -or $headers -notmatch '(?im)^\s*AA64 machine') { throw 'viogpud3dx.dll is not an ARM64 machine image' }
if ($headers -notmatch '\.a64xrm' -or $headers -notmatch '\.hexpthk') { throw 'viogpud3dx.dll has no ARM64X hybrid view' }
$native = (& dumpbin /exports "$out/viogpud3dx.dll") -join "`n"
if ($LASTEXITCODE) { throw 'Cannot read native exports' }
foreach ($name in $exports) {
    if ($native -notmatch ('(?m)\s' + [regex]::Escape($name) + '\s*$')) { throw "Native view lacks export $name" }
}
$imports = (& dumpbin /dependents "$out/viogpud3dx.dll") -join "`n"
if ($imports -match '(?i)\b(?:msvcp|vcruntime)[0-9_]*\.dll\b|viogpud3d(?:_x64)?\.dll') {
    throw 'viogpud3dx.dll must not import the CRT or statically import its Mesa target'
}
Get-ChildItem $out -File -Filter '*.exe' | ForEach-Object {
    $machine = @{ 'd3d-umd-probe-arm64.exe' = 'AA64'; 'd3d-umd-probe-x64.exe' = '8664'; 'd3d-umd-probe-x86.exe' = '14C' }[$_.Name]
    $text = (& dumpbin /headers $_.FullName) -join "`n"
    if (!$machine -or $text -notmatch "(?im)^\s*$machine machine") { throw "Wrong probe architecture: $($_.Name)" }
}
Write-Host 'PASS ARM64X D3D UMD entry and arm64/x64/x86 load probes built; loading is proven on the ARM64 runner'
