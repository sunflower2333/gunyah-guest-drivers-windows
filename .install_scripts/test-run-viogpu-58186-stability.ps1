$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$runnerPath = Join-Path $PSScriptRoot 'run-viogpu-58186-stability.ps1'
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    $runnerPath,
    [ref]$tokens,
    [ref]$parseErrors
)
if ($parseErrors.Count -ne 0) {
    throw "Stability parser failed: $($parseErrors.Message -join '; ')"
}

$requiredFunctions = @('ConvertTo-U32', 'Test-GpuSnapshot', 'Test-DestroyDiagnostics')
$functionAsts = @($ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst]
}, $true) | Where-Object { $_.Name -in $requiredFunctions })
foreach ($name in $requiredFunctions) {
    $function = @($functionAsts | Where-Object { $_.Name -eq $name })
    if ($function.Count -ne 1) { throw "Expected one '$name' function, found $($function.Count)." }
    Invoke-Expression $function[0].Extent.Text
}

$expectedDriverVersion = '100.6.101.58186'
$expectedDriverHash = 'd3f920cb6a5367b468b831fd10d80308fd1272a5e2f32dc29a8a863442a91be2'
$expectedUmdHash = 'a65d1abeec1860fe9a8f8be58a53e6f71f8a635659dcbd42bb71b98b8a452754'
$expectedSignerSubject = 'CN=DroidVM Test'
$destroyValueSuffixes = @(
    'Attempt', 'Status', 'Detail', 'HostResult', 'ContextId',
    'ContextState', 'OwnerState', 'Released', 'Retrying', 'OwnerRetained'
)

$gpu = [pscustomobject]@{
    Status = 'OK'
    ProblemCode = 0
    InstanceId = 'PCI\VEN_1AF4&DEV_1050\fixture'
    DriverVersion = $expectedDriverVersion
    DriverImageSha256 = $expectedDriverHash
    UmdSha256 = $expectedUmdHash
    SignatureStatus = 'Valid'
    SignerSubject = $expectedSignerSubject
    DriverServiceStatus = 'Running'
}
$gpuCheck = Test-GpuSnapshot $gpu $gpu.InstanceId
if (-not $gpuCheck.Passed -or $gpuCheck.Errors.Count -ne 0) {
    throw "Healthy GPU fixture failed: $($gpuCheck | ConvertTo-Json -Depth 4)"
}
$badGpu = $gpu | Select-Object *
$badGpu.DriverImageSha256 = 'bad'
$badGpu.InstanceId = 'changed'
$badGpuCheck = Test-GpuSnapshot $badGpu $gpu.InstanceId
if ($badGpuCheck.Passed -or $badGpuCheck.Errors.Count -ne 2) {
    throw "Unhealthy GPU fixture was accepted: $($badGpuCheck | ConvertTo-Json -Depth 4)"
}

$cleanDiagnostics = [ordered]@{
    NativeContextDestroySlot02Attempt = 1
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
$cleanCheck = Test-DestroyDiagnostics $cleanDiagnostics
if (-not $cleanCheck.Passed -or $cleanCheck.SlotCount -ne 1) {
    throw "Clean destroy fixture failed: $($cleanCheck | ConvertTo-Json -Depth 4)"
}
$cleanDiagnostics.NativeContextDestroySlot02OwnerRetained = 1
$retainedCheck = Test-DestroyDiagnostics $cleanDiagnostics
if ($retainedCheck.Passed -or $retainedCheck.Errors.Count -ne 1) {
    throw "Retained destroy fixture was accepted: $($retainedCheck | ConvertTo-Json -Depth 4)"
}

$runnerText = [IO.File]::ReadAllText($runnerPath)
foreach ($fragment in @(
    "[ValidateSet('Baseline', 'Final')]",
    'MinimumObservationMinutes = 30',
    'kmt-summary.json',
    'turnip-summary.json',
    'negative-summary.json',
    '$requiredObservationMinutes = [math]::Max',
    'Windows rebooted after the stability baseline.',
    'Suite evidence changed after the stability baseline.',
    'Crash-dump inventory changed after the stability baseline.',
    'new critical system/application events',
    'C:\Windows\LiveKernelReports'
)) {
    if ($runnerText.IndexOf($fragment, [StringComparison]::Ordinal) -lt 0) {
        throw "Stability runner is missing exact contract fragment: $fragment"
    }
}

Write-Host 'viogpu 58186 stability wrapper fixtures: PASS'
