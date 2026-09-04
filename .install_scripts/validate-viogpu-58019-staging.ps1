[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$homePath = 'C:\Users\Administrator'
$packagePath = Join-Path $homePath 'viogpuwddm-58019'
$expectedHashes = [ordered]@{
    'viogpuwddm.sys' = 'baccee04e10802b4331f92b4a8cbe8a9e8e38867918264a152c65cb2d013a6ca'
    'viogpud3d.dll' = '1ead18f1b8ab182eb2ced08551e87288638edd02c660e839ca293067c60d82d0'
    'viogpuwddm.cat' = 'c7a31701f44e4c755baf3b600e0cf05f115252161f405575591b8511184b91d7'
    'viogpuwddm.inf' = 'd34ac813dd025592f9e9ae06333f43ed0a2ced93c86f61a7bd17f3f19065f4cf'
}

$actualHashes = [ordered]@{}
foreach ($entry in $expectedHashes.GetEnumerator()) {
    $filePath = Join-Path $packagePath $entry.Key
    $actual = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $entry.Value) {
        throw "SHA-256 mismatch for '$filePath': expected $($entry.Value), got $actual."
    }
    $actualHashes[$entry.Key] = $actual
}

$infPath = Join-Path $packagePath 'viogpuwddm.inf'
$infText = Get-Content -LiteralPath $infPath -Raw
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58019') {
    throw "Staged INF '$infPath' is not version 100.6.101.58019."
}

$scriptNames = @(
    'install-viogpu-runtime-package.ps1',
    'install-viogpu-runtime-with-trace.ps1',
    'prepare-viogpu-58019-test.ps1',
    'capture-viogpu-display-restart.ps1',
    'analyze-viogpu-runtime-etl.ps1',
    'viogpu-runtime-preflight.ps1',
    'viogpu-runtime-display-state.ps1'
)
foreach ($scriptName in $scriptNames) {
    $tokens = $null
    $errors = $null
    $scriptPath = Join-Path $homePath $scriptName
    [void][System.Management.Automation.Language.Parser]::ParseFile(
        $scriptPath,
        [ref]$tokens,
        [ref]$errors
    )
    if ($errors.Count -ne 0) {
        throw "PowerShell parse failed for '$scriptPath': $($errors -join '; ')"
    }
}

[pscustomobject]@{
    ValidatedAt = (Get-Date).ToString('o')
    PackageVersion = '100.6.101.58019'
    PackagePath = $packagePath
    Hashes = $actualHashes
    ParsedScripts = $scriptNames
} | ConvertTo-Json -Depth 4
