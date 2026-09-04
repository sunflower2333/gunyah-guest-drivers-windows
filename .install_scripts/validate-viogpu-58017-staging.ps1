[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$homePath = 'C:\Users\Administrator'
$packagePath = Join-Path $homePath 'viogpuwddm-58017'
$expectedHashes = [ordered]@{
    'viogpuwddm.sys' = '56483a5b3e6244d85dd9b6bab4968fe74e234fb2b26155f9955a859061cb6a7f'
    'viogpud3d.dll' = '5453b9186e296595b116dee9c4a9191657fefb218bf5a3d3dc39998c09b0cd94'
    'viogpuwddm.cat' = '0291ea326d0574c844a276278cd786199bedacec28278b2483647029e08d4978'
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
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58017') {
    throw "Staged INF '$infPath' is not version 100.6.101.58017."
}

$scriptNames = @(
    'install-viogpu-runtime-package.ps1',
    'prepare-viogpu-58017-test.ps1',
    'capture-viogpu-display-restart.ps1',
    'analyze-viogpu-runtime-etl.ps1',
    'viogpu-native-present-diagnostics.ps1',
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
    PackageVersion = '100.6.101.58017'
    PackagePath = $packagePath
    Hashes = $actualHashes
    ParsedScripts = $scriptNames
} | ConvertTo-Json -Depth 4
