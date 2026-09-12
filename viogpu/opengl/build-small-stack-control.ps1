# SPDX-License-Identifier: MIT
# Reproduce the former 128 KiB automatic path storage while keeping every
# current flat dependency name and initialization step identical.
param([string]$Output = 'opengl-legacy')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$source = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'icd-proxy.cpp'))
$start = $source.IndexOf('struct ScopedModulePaths')
$end = $source.IndexOf('static HMODULE load_sibling', $start)
if ($start -lt 0 -or $end -lt 0) { throw 'Proxy storage source changed' }
$old = $source.Substring($start, $end - $start)
if ($old -notmatch 'HeapAlloc' -or $old -notmatch 'HeapFree' -or $old -notmatch 'SetLastError') {
    throw 'Expected fixed heap-storage implementation not found'
}
$control = @'
struct ScopedModulePaths
{
    ModulePaths automatic;
    ModulePaths *value = &automatic;
};

'@
$path = Join-Path $env:RUNNER_TEMP 'viogpu-flat-stack-control.cpp'
[IO.File]::WriteAllText($path, $source.Remove($start, $end - $start).Insert($start, $control))
& "$PSScriptRoot/build-proxy.ps1" -Output $Output -ProxySource $path
