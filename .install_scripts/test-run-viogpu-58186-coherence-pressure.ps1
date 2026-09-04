$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$runnerPath = Join-Path $PSScriptRoot 'run-viogpu-58186-coherence-pressure.ps1'
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

$requiredFunctions = @(
    'Measure-PressureDdiEvents',
    'Test-CoherencePressureEvidence',
    'Get-PressureArgumentLine'
)
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
    'Microsoft-Windows-DxgKrnl, Stop , 106, "DdiSubmitCommand "'
)
$counts = Measure-PressureDdiEvents $lines
foreach ($name in @('DdiBuildPagingBuffer', 'DdiPatch', 'DdiSubmitCommand')) {
    if ($counts[$name].Start -ne 1 -or $counts[$name].Stop -ne 1 -or $counts[$name].Total -ne 2) {
        throw "Balanced count fixture failed for ${name}: $($counts[$name] | ConvertTo-Json -Compress)"
    }
}

$stdout = @'
tu WDDM Vulkan compute probe passed: Turnip Adreno, elements 67108864, checksum 144115183613116416
tu WDDM Vulkan coherence pressure passed: elements 67108864, iterations 16, bytes 268435456, host-cached 1, host-coherent 1
'@
$passing = Test-CoherencePressureEvidence $counts $stdout '' 0 $false
if (-not $passing.Passed -or -not $passing.ExactLargeBufferCoherenceObserved -or
    -not $passing.BalancedPagingDdiActivityObserved) {
    throw "Passing pressure fixture failed: $($passing | ConvertTo-Json -Depth 6)"
}
if ($passing.ForcedPageOutPageInProven -or $passing.MultipassPrivateRecordRetentionProven -or
    $passing.PartialPagingFailureRecoveryProven) {
    throw 'Pressure evidence incorrectly claimed an unobservable paging or recovery gate.'
}

$unbalanced = [ordered]@{}
foreach ($name in $counts.Keys) {
    $unbalanced[$name] = [ordered]@{
        Start = $counts[$name].Start
        Stop = $counts[$name].Stop
        Total = $counts[$name].Total
    }
}
$unbalanced.DdiBuildPagingBuffer.Stop = 0
$failedTrace = Test-CoherencePressureEvidence $unbalanced $stdout '' 0 $false
if ($failedTrace.Passed -or
    $failedTrace.Errors -notcontains 'Trace has unbalanced DdiBuildPagingBuffer events: start=1 stop=0.') {
    throw "Unbalanced paging DDI fixture was accepted: $($failedTrace | ConvertTo-Json -Depth 6)"
}

$failedChecksum = Test-CoherencePressureEvidence $counts ($stdout -replace '144115183613116416', '1') '' 0 $false
if ($failedChecksum.Passed -or $failedChecksum.ExactLargeBufferCoherenceObserved) {
    throw 'Incorrect checksum marker fixture was accepted.'
}
$duplicate = Test-CoherencePressureEvidence $counts ($stdout + $stdout) '' 0 $false
if ($duplicate.Passed) {
    throw 'Duplicate success markers were accepted.'
}
$failedStderr = Test-CoherencePressureEvidence $counts $stdout 'unexpected diagnostic' 0 $false
if ($failedStderr.Passed) {
    throw 'Nonempty stderr was accepted.'
}

$argumentLine = Get-PressureArgumentLine 'C:\Pair Root\tu_wddm_compute.spv'
$expectedArgumentLine = '"C:\Pair Root\tu_wddm_compute.spv" --elements 67108864 --iterations 16'
if ($argumentLine -ne $expectedArgumentLine) {
    throw "Pressure argument quoting fixture failed: $argumentLine"
}
try {
    $null = Get-PressureArgumentLine 'C:\bad"shader.spv'
    throw 'Quoted shader path was accepted.'
} catch {
    if ($_.Exception.Message -ne 'Shader path must not contain double quotes.') {
        throw
    }
}

Write-Host 'viogpu 58186 coherence-pressure observer fixtures: PASS'
