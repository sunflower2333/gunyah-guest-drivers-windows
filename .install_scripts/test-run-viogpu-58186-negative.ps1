$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$runnerPath = Join-Path $PSScriptRoot 'run-viogpu-58186-negative.ps1'
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
    'Test-NegativeMarkers',
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

$runnerText = [IO.File]::ReadAllText($runnerPath)
foreach ($fragment in @(
    'ce197d78a2ef8e0ec884505063d242c5adbb28d7705ae474144e060720c31030',
    '--negative-iova',
    '--negative-submit',
    '--submit-nop',
    'Negative IOVA summary: malformed=1 out_of_range=1 overlap=1 post=1 active=1',
    'Negative submit summary: status=0xc000001d replacements=1 cleaned=1 active=1 pass=1'
)) {
    if ($runnerText.IndexOf($fragment, [StringComparison]::Ordinal) -lt 0) {
        throw "Runner is missing exact negative contract fragment: $fragment"
    }
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

$negativeOutput = @'
  Negative IOVA summary: malformed=1 out_of_range=1 overlap=1 post=1 active=1
  Negative submit summary: status=0xc000001d replacements=1 cleaned=1 active=1 pass=1
  Submit probe Render(NOP): success=1 fence=7
  Submit probe fence: completed=1 query=1 value=7
tu WDDM KMT probe passed stage=allocation
'@
$negativeCheck = Test-NegativeMarkers $negativeOutput ''
if (-not $negativeCheck.Passed -or $negativeCheck.SubmittedFence -ne 7 -or $negativeCheck.CompletedFence -ne 7) {
    throw "Passing negative fixture failed: $($negativeCheck | ConvertTo-Json -Depth 4)"
}
$badIovaCheck = Test-NegativeMarkers ($negativeOutput -replace 'overlap=1', 'overlap=0') ''
if ($badIovaCheck.Passed -or $badIovaCheck.Errors.Count -ne 1) {
    throw "Bad IOVA summary fixture was accepted: $($badIovaCheck | ConvertTo-Json -Depth 4)"
}
$badSubmitCheck = Test-NegativeMarkers ($negativeOutput -replace 'replacements=1', 'replacements=0') ''
if ($badSubmitCheck.Passed -or $badSubmitCheck.Errors.Count -ne 1) {
    throw "Bad submit summary fixture was accepted: $($badSubmitCheck | ConvertTo-Json -Depth 4)"
}
$setupFailureCheck = Test-NegativeMarkers ($negativeOutput + "`n  Negative submit allocation setup: created=0 cleaned=0 handle=0x00000001") ''
if ($setupFailureCheck.Passed -or $setupFailureCheck.Errors.Count -ne 1) {
    throw "Allocation setup failure fixture was accepted: $($setupFailureCheck | ConvertTo-Json -Depth 4)"
}
$mismatchCheck = Test-NegativeMarkers ($negativeOutput -replace 'value=7', 'value=6') ''
if ($mismatchCheck.Passed -or $mismatchCheck.Errors.Count -ne 1) {
    throw "Mismatched-fence fixture was accepted: $($mismatchCheck | ConvertTo-Json -Depth 4)"
}
$stderrCheck = Test-NegativeMarkers $negativeOutput 'unexpected stderr'
if ($stderrCheck.Passed -or $stderrCheck.Errors.Count -ne 1) {
    throw "Nonempty-stderr fixture was accepted: $($stderrCheck | ConvertTo-Json -Depth 4)"
}

if ((ConvertTo-U32 -1073741661) -ne 3221225635L) {
    throw 'Signed NTSTATUS conversion fixture failed.'
}

Write-Host 'viogpu 58186 negative wrapper fixtures: PASS'
