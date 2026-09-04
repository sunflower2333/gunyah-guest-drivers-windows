[CmdletBinding()]
param(
    [string]$RunnerPath = (Join-Path $PSScriptRoot 'run-viogpu-58186-kmt.ps1'),
    [string]$ExpectedLabel = '58186'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

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
    'ConvertTo-U32',
    'Test-DestroyDiagnostics',
    'Test-StressMarkers',
    'Test-SubmitMarkers'
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

$destroyValueSuffixes = @(
    'Attempt',
    'Status',
    'Detail',
    'HostResult',
    'ContextId',
    'ContextState',
    'OwnerState',
    'Released',
    'Retrying',
    'OwnerRetained'
)
$cleanDiagnostics = [ordered]@{
    NativeContextDestroySlot02Attempt = 12
    NativeContextDestroySlot02Stage = 0x0FFF
    NativeContextDestroySlot02Status = 0
    NativeContextDestroySlot02Detail = 0
    NativeContextDestroySlot02HostResult = 1
    NativeContextDestroySlot02ContextId = 66
    NativeContextDestroySlot02ContextState = 4
    NativeContextDestroySlot02OwnerState = 2
    NativeContextDestroySlot02Released = 1
    NativeContextDestroySlot02Retrying = 0
    NativeContextDestroySlot02OwnerRetained = 0
}
$cleanCheck = Test-DestroyDiagnostics $cleanDiagnostics 'fixture-clean'
if (-not $cleanCheck.Passed -or $cleanCheck.SlotCount -ne 1 -or $cleanCheck.Errors.Count -ne 0) {
    throw "Clean destroy fixture failed: $($cleanCheck | ConvertTo-Json -Depth 6)"
}

$cleanDiagnostics.NativeContextDestroySlot02OwnerRetained = 1
$retainedCheck = Test-DestroyDiagnostics $cleanDiagnostics 'fixture-retained'
if ($retainedCheck.Passed -or $retainedCheck.Errors.Count -ne 1) {
    throw "Retained-owner fixture was accepted: $($retainedCheck | ConvertTo-Json -Depth 6)"
}

$stressOutput = @'
  Stress lifecycle: DestroyAllocation iteration=0 attempt=1 success=1 status=0x00000000 handle=0x00000000
  Stress lifecycle: passed iterations=10000
  Stress context lifecycle: passed iterations=10000
  Stress lifecycle summary: allocation_before=1 context_only=1 allocation_after=1
tu WDDM KMT probe passed stage=allocation
'@
$stressCheck = Test-StressMarkers $stressOutput ''
if (-not $stressCheck.Passed) {
    throw "Passing lifecycle fixture failed: $($stressCheck.Errors -join '; ')"
}
$roundOneBusyOutput = $stressOutput + "`n  Stress lifecycle: DestroyAllocation iteration=1 attempt=1 success=0 status=0xC000009E handle=0x00000001"
$busyCheck = Test-StressMarkers $roundOneBusyOutput ''
if ($busyCheck.Passed -or $busyCheck.Errors -notcontains 'Lifecycle output contains a round-1 allocation destroy failure.') {
    throw "Round-1 busy fixture was accepted: $($busyCheck | ConvertTo-Json -Depth 4)"
}

$submitOutput = @'
  Submit probe Render(NOP): success=1 fence=7
  Submit probe fence: completed=1 query=1 value=7
tu WDDM KMT probe passed stage=allocation
'@
$submitCheck = Test-SubmitMarkers $submitOutput ''
if (-not $submitCheck.Passed -or $submitCheck.SubmittedFence -ne 7 -or $submitCheck.CompletedFence -ne 7) {
    throw "Passing submit fixture failed: $($submitCheck | ConvertTo-Json -Depth 4)"
}
$mismatchCheck = Test-SubmitMarkers ($submitOutput -replace 'value=7', 'value=6') ''
if ($mismatchCheck.Passed -or $mismatchCheck.Errors.Count -ne 1) {
    throw "Mismatched-fence fixture was accepted: $($mismatchCheck | ConvertTo-Json -Depth 4)"
}

if ((ConvertTo-U32 -1073741661) -ne 3221225635L) {
    throw 'Signed NTSTATUS conversion fixture failed.'
}

Write-Host "viogpu $ExpectedLabel KMT wrapper fixtures: PASS"
