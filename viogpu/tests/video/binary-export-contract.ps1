# SPDX-License-Identifier: BSD-3-Clause
# Exercise the actual PowerShell export gates embedded in both production workflows.
[CmdletBinding()]
param([string]$Umd)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$names = @(
    'OpenAdapter', 'OpenAdapter10', 'OpenAdapter10_2',
    'VioGpuVideoOpen', 'VioGpuVideoControl', 'VioGpuVideoAllocate',
    'VioGpuVideoQueue', 'VioGpuVideoDequeue', 'VioGpuVideoCopy',
    'VioGpuVideoStream', 'VioGpuVideoClose'
)

# Generate the dumpbin row shape used by the production parser, not a second validator.
function New-ExportFixture {
    param([string[]]$Names)
    $rows = @('Dump of file export-fixture.dll', '  ordinal hint RVA      name')
    for ($i = 0; $i -lt $Names.Count; $i++) {
        $rows += ('      {0} {1:X4} {2:X8} {3}' -f ($i + 1), $i, (0x1000 + $i * 16), $Names[$i])
    }
    return ($rows -join "`n")
}

# Accept a negative control only when the production export check rejects it for the expected reason.
function Test-ExportGate {
    param(
        [scriptblock]$Gate,
        [string]$Text,
        [bool]$ExpectedSuccess,
        [string]$Label
    )
    $failure = $null
    try { & $Gate $Text 6>$null | Out-Null }
    catch { $failure = $_.Exception.Message }
    if ($ExpectedSuccess -and $null -ne $failure) { throw "$Label unexpectedly failed: $failure" }
    if (-not $ExpectedSuccess) {
        if ($null -eq $failure) { throw "$Label was incorrectly accepted" }
        if ($failure -notmatch 'UMD export table mismatch|legacy UMD shim must not export OpenAdapter12') {
            throw "$Label failed outside the export gate: $failure"
        }
    }
}

$actual = $null
if ($Umd) {
    $path = (Resolve-Path -LiteralPath $Umd -ErrorAction Stop).Path
    $dumpbin = (Get-Command dumpbin.exe -ErrorAction Stop).Source
    $actual = (& $dumpbin /nologo /exports $path 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed for $path" }
}
foreach ($workflow in @('viogpuwddm-arm64-ci.yml', 'build-arm64-drivers.yml')) {
    $source = [IO.File]::ReadAllText((Join-Path $root ".github/workflows/$workflow"))
    $pattern = '(?ms)^          \$exportNames = @\(\r?\n.*?^          if \(\$exportNames -contains ''OpenAdapter12''\) \{ throw ''legacy UMD shim must not export OpenAdapter12'' \}\r?$'
    $matches = [regex]::Matches($source, $pattern)
    if ($matches.Count -ne 1) { throw "No unique production export gate in $workflow" }
    $body = [regex]::Replace($matches[0].Value, '(?m)^          ', '')
    $gate = [scriptblock]::Create('param([string]$exports)' + "`n" + $body)
    Test-ExportGate $gate (New-ExportFixture $names) $true "$workflow complete table"
    Test-ExportGate $gate (New-ExportFixture @($names | Sort-Object -Descending)) $true "$workflow reordered table"
    foreach ($missing in $names) {
        Test-ExportGate $gate (New-ExportFixture @($names | Where-Object { $_ -cne $missing })) $false "$workflow missing $missing"
    }
    foreach ($extra in @('OpenAdapter12', 'VioGpuVideoUnknown')) {
        Test-ExportGate $gate (New-ExportFixture @($names + $extra)) $false "$workflow unexpected $extra"
    }
    Test-ExportGate $gate (New-ExportFixture @($names + $names[0])) $false "$workflow duplicate export"
    Test-ExportGate $gate 'not a valid export table' $false "$workflow malformed table"
    if ($null -ne $actual) { Test-ExportGate $gate $actual $true "$workflow built UMD" }
    Write-Host "PASS: $workflow production export gate"
    Write-Host '  11 missing-function controls, 2 unknown functions, duplicate and malformed tables rejected.'
    if ($null -ne $actual) { Write-Host '  Actual built DLL export table accepted.' }
}
