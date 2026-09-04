$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$runnerPath = Join-Path $PSScriptRoot 'rollback-viogpu-58186-to-58182.ps1'
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    $runnerPath,
    [ref]$tokens,
    [ref]$parseErrors
)
if ($parseErrors.Count -ne 0) {
    throw "Rollback parser failed: $($parseErrors.Message -join '; ')"
}

$gpuCheckAst = @($ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Test-GpuSnapshot'
}, $true))
if ($gpuCheckAst.Count -ne 1) {
    throw "Expected one Test-GpuSnapshot function, found $($gpuCheckAst.Count)."
}
Invoke-Expression $gpuCheckAst[0].Extent.Text

$expectedSignerSubject = 'CN=DroidVM Test'
$currentVersion = '100.6.101.58186'
$currentSys = 'd3f920cb6a5367b468b831fd10d80308fd1272a5e2f32dc29a8a863442a91be2'
$currentUmd = 'a65d1abeec1860fe9a8f8be58a53e6f71f8a635659dcbd42bb71b98b8a452754'
$good = [pscustomobject]@{
    Status = 'OK'
    ProblemCode = 0
    InstanceId = 'PCI\VEN_1AF4&DEV_1050\fixture'
    DriverVersion = $currentVersion
    ImageSha256 = $currentSys
    UmdSha256 = $currentUmd
    SignatureStatus = 'Valid'
    SignerSubject = $expectedSignerSubject
    DriverServiceStatus = 'Running'
}
$goodCheck = Test-GpuSnapshot $good $currentVersion $currentSys $currentUmd $good.InstanceId
if (-not $goodCheck.Passed -or $goodCheck.Errors.Count -ne 0) {
    throw "Healthy snapshot fixture failed: $($goodCheck | ConvertTo-Json -Depth 4)"
}

$bad = $good | Select-Object *
$bad.ProblemCode = 43
$bad.DriverVersion = '100.6.101.58185'
$bad.SignatureStatus = 'NotSigned'
$bad.DriverServiceStatus = 'Stopped'
$badCheck = Test-GpuSnapshot $bad $currentVersion $currentSys $currentUmd $good.InstanceId
if ($badCheck.Passed -or $badCheck.Errors.Count -ne 4) {
    throw "Unhealthy snapshot fixture was accepted: $($badCheck | ConvertTo-Json -Depth 4)"
}

$runnerText = [IO.File]::ReadAllText($runnerPath)
foreach ($fragment in @(
    '100.6.101.58186',
    '100.6.101.58182',
    'd3f920cb6a5367b468b831fd10d80308fd1272a5e2f32dc29a8a863442a91be2',
    'a65d1abeec1860fe9a8f8be58a53e6f71f8a635659dcbd42bb71b98b8a452754',
    'bd862478c4dbf81da0204f2004fb96f2f1d005ab38bdfcde3d794fedfd28bb3e',
    'ada59e6f4b6aede076bd661a591d9490396472ed2fc92c3b5cd1d654eb33c2eb',
    '91438a009c932da66e4ae232f864488c7aedfab8d08cc499bc8dfa4108a9eec9',
    'a6c1009691958bec6df629752f12beb190a95cc8e0d138548247bd8e0ade7229',
    '$files.Count -ne $expectedRollbackHashes.Count',
    '$hash -ne $expectedRollbackHashes[[string]$entry.Name]',
    '$success = $rollbackExitCode -eq 0 -and $afterCheck.Passed',
    'rollback-58186-to-58182-result.json'
)) {
    if ($runnerText.IndexOf($fragment, [StringComparison]::Ordinal) -lt 0) {
        throw "Rollback runner is missing exact contract fragment: $fragment"
    }
}

Write-Host 'viogpu 58186 to 58182 rollback fixtures: PASS'
