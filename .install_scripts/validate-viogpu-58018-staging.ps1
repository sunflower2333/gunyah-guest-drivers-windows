[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$homePath = 'C:\Users\Administrator'
$packagePath = Join-Path $homePath 'viogpuwddm-58018'
$expectedHashes = [ordered]@{
    'viogpuwddm.sys' = '4f9bf919d63ef738d32cef9481b28a7de7720dc604f99729fe41f05e927b71f5'
    'viogpud3d.dll' = 'b43dc06ebeab5495494d93afd2ebbdd3e597dddb3c06d53193dc45aae071432c'
    'viogpuwddm.cat' = 'ee4fb8fe84ad99e647e4084f3974b0385bf460bf0d1d097ca1f38567ff47f75c'
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
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58018') {
    throw "Staged INF '$infPath' is not version 100.6.101.58018."
}

$scriptNames = @(
    'install-viogpu-runtime-package.ps1',
    'prepare-viogpu-58018-test.ps1',
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
    PackageVersion = '100.6.101.58018'
    PackagePath = $packagePath
    Hashes = $actualHashes
    ParsedScripts = $scriptNames
} | ConvertTo-Json -Depth 4
