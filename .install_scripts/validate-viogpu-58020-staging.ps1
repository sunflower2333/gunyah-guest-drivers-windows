[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$homePath = 'C:\Users\Administrator'
$packagePath = Join-Path $homePath 'viogpuwddm-58020'
$expectedHashes = [ordered]@{
    'viogpuwddm.sys' = '21bb6445dffcd143bc996ee24156fd280e11cd6dd85ff393c15f0246ae74434f'
    'viogpud3d.dll' = 'e7b13323ca8d033e193dba0ef56d786265a4ec221369b746bdc240499b460188'
    'viogpuwddm.cat' = '209505ba9e532954cd6849dfccf8e436cbf00251d8fe048c196a7064eeb5bded'
    'viogpuwddm.inf' = 'fde7d3f4b55b72a74435b2d1b3271a08f25d1c75f400966ca15081a1274dbf7c'
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
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58020') {
    throw "Staged INF '$infPath' is not version 100.6.101.58020."
}

$scriptNames = @(
    'install-viogpu-runtime-package.ps1',
    'install-viogpu-runtime-with-trace.ps1',
    'prepare-viogpu-58020-test.ps1',
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
    PackageVersion = '100.6.101.58020'
    PackagePath = $packagePath
    Hashes = $actualHashes
    ParsedScripts = $scriptNames
} | ConvertTo-Json -Depth 4
