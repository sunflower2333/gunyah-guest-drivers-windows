$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$runnerPath = Join-Path $PSScriptRoot 'run-viogpu-58186-paging-present.ps1'
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    $runnerPath,
    [ref]$tokens,
    [ref]$parseErrors
)
if ($parseErrors.Count -ne 0) {
    throw "Runner parse failed: $($parseErrors.Message -join '; ')"
}

$requiredFunctions = @('Measure-DdiEvents', 'Test-NormalPagingPresentEvidence', 'Get-ChildArgumentLine')
$functionAsts = @($ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst]
}, $true) | Where-Object { $_.Name -in $requiredFunctions })
foreach ($name in $requiredFunctions) {
    $function = @($functionAsts | Where-Object { $_.Name -eq $name })
    if ($function.Count -ne 1) {
        throw "Expected exactly one '$name' function, found $($function.Count)."
    }
    Invoke-Expression $function[0].Extent.Text
}

$lines = @(
    'Microsoft-Windows-DxgKrnl, Start , 105, "DdiBuildPagingBuffer "',
    'Microsoft-Windows-DxgKrnl, Stop , 106, "DdiBuildPagingBuffer "',
    'Microsoft-Windows-DxgKrnl, Start , 105, "DdiPatch "',
    'Microsoft-Windows-DxgKrnl, Stop , 106, "DdiPatch "',
    'Microsoft-Windows-DxgKrnl, Start , 105, "DdiSubmitCommand "',
    'Microsoft-Windows-DxgKrnl, Stop , 106, "DdiSubmitCommand "',
    'Microsoft-Windows-DxgKrnl, Start , 105, "DdiPresent "',
    'Microsoft-Windows-DxgKrnl, Stop , 106, "DdiPresent "'
)
$counts = Measure-DdiEvents $lines
foreach ($name in @('DdiBuildPagingBuffer', 'DdiPatch', 'DdiSubmitCommand', 'DdiPresent')) {
    if ($counts[$name].Start -ne 1 -or $counts[$name].Stop -ne 1 -or $counts[$name].Total -ne 2) {
        throw "Balanced count fixture failed for ${name}: $($counts[$name] | ConvertTo-Json -Compress)"
    }
}

$turnipSummary = [pscustomobject]@{
    Success = $true
    DirectProbeResults = @(
        [pscustomobject]@{ Name = 'compute'; Success = $true },
        [pscustomobject]@{ Name = 'graphics'; Success = $true }
    )
    InteractiveResult = [pscustomobject]@{ Success = $true }
}
$passing = Test-NormalPagingPresentEvidence $counts $turnipSummary
if (-not $passing.Passed -or -not $passing.BuiltPagingToSubmitObserved -or
    -not $passing.KmdPresentActivityObserved -or -not $passing.WorkloadChecksumCoherenceObserved -or
    -not $passing.InteractiveVisiblePresentObserved) {
    throw "Passing evidence fixture failed: $($passing | ConvertTo-Json -Depth 6)"
}
if ($passing.MultipassPrivateRecordRetentionProven -or $passing.PartialPagingFailureRecoveryProven) {
    throw 'Normal-path evidence incorrectly claimed a recovery-specific gate.'
}

$unbalanced = [ordered]@{}
foreach ($name in $counts.Keys) {
    $unbalanced[$name] = [ordered]@{
        Start = $counts[$name].Start
        Stop = $counts[$name].Stop
        Total = $counts[$name].Total
    }
}
$unbalanced.DdiPatch.Stop = 0
$failed = Test-NormalPagingPresentEvidence $unbalanced $turnipSummary
if ($failed.Passed -or $failed.Errors -notcontains 'Trace has unbalanced DdiPatch events: start=1 stop=0.') {
    throw "Unbalanced DDI fixture was accepted: $($failed | ConvertTo-Json -Depth 6)"
}

$turnipSummary.DirectProbeResults[1].Success = $false
$failedChecksum = Test-NormalPagingPresentEvidence $counts $turnipSummary
if ($failedChecksum.Passed -or $failedChecksum.WorkloadChecksumCoherenceObserved) {
    throw 'Failed graphics checksum fixture was accepted.'
}

$argumentLine = Get-ChildArgumentLine 'C:\Program Files\turnip.ps1' 'C:\Bundle Root' `
    'C:\Tools\interactive.ps1' 'C:\Tools\capture.ps1' 'C:\Evidence Root'
$expectedArgumentLine = '-NoProfile -ExecutionPolicy Bypass -File "C:\Program Files\turnip.ps1" ' +
    '-BundleRoot "C:\Bundle Root" -InteractiveRunnerPath "C:\Tools\interactive.ps1" ' +
    '-CaptureScriptPath "C:\Tools\capture.ps1" -OutputDirectory "C:\Evidence Root"'
if ($argumentLine -ne $expectedArgumentLine) {
    throw "Child argument quoting fixture failed: $argumentLine"
}
try {
    $null = Get-ChildArgumentLine 'C:\bad"runner.ps1' 'C:\Bundle' 'C:\interactive.ps1' `
        'C:\capture.ps1' 'C:\evidence'
    throw 'Quoted child path was accepted.'
} catch {
    if ($_.Exception.Message -ne 'Child paths must not contain double quotes.') {
        throw
    }
}

Write-Host 'viogpu 58186 paging/Present observer fixtures: PASS'
