[CmdletBinding()]
param(
    [string]$RunnerPath = (Join-Path $PSScriptRoot 'run-viogpu-58186-turnip.ps1'),
    [string]$ExpectedLabel = '58186'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$runnerText = Get-Content -LiteralPath $runnerPath -Raw
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
if ($runnerText -notmatch '(?s)if \(\$arguments\.Count -ne 0\).*?\.ArgumentList = \$arguments') {
    throw 'Runner does not omit Start-Process ArgumentList for an empty probe argument array.'
}
if ($runnerText -notmatch '\$newOrChangedDumps\s*=\s*@\(Get-NewOrChangedDumps') {
    throw 'Runner does not preserve an empty dump-difference result as an array.'
}
if ($runnerText -notmatch '\[switch\]\$SkipInteractive') {
    throw 'Runner does not expose the explicit SkipInteractive switch.'
}
if ($runnerText -notmatch '(?s)\$scriptInputs\s*=\s*if \(\$SkipInteractive\)\s*\{\s*@\(\)\s*\}\s*else') {
    throw 'Runner does not skip interactive-helper validation in direct-only mode.'
}
if ($runnerText -notmatch '(?s)if \(\$suiteErrors\.Count -eq 0 -and -not \$SkipInteractive\).*?Invoke-InteractiveProbe') {
    throw 'Runner does not guard the interactive probe with SkipInteractive.'
}

$requiredFunctions = @(
    'ConvertTo-U32',
    'Assert-ExactBundle',
    'Test-DestroyDiagnostics',
    'Get-DumpSnapshot',
    'Get-NewOrChangedDumps',
    'Get-FaultEvents',
    'Get-DirectProbeSpecs',
    'Test-DirectProbeOutput',
    'Get-InteractiveArgumentLine'
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

$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('viogpu-58186-turnip-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
try {
    $firstPath = Join-Path $fixtureRoot 'first.bin'
    $secondPath = Join-Path $fixtureRoot 'second.bin'
    [IO.File]::WriteAllText($firstPath, 'first fixture')
    [IO.File]::WriteAllText($secondPath, 'second fixture')
    $expectedBundleHashes = [ordered]@{
        'first.bin' = (Get-FileHash -LiteralPath $firstPath -Algorithm SHA256).Hash.ToLowerInvariant()
        'second.bin' = (Get-FileHash -LiteralPath $secondPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $BundleRoot = $fixtureRoot
    $bundle = Assert-ExactBundle
    if ($bundle.EntryCount -ne 2 -or ($bundle.Entries -join ',') -ne 'first.bin,second.bin') {
        throw "Exact bundle fixture failed: $($bundle | ConvertTo-Json -Depth 4)"
    }

    $extraPath = Join-Path $fixtureRoot 'extra.bin'
    [IO.File]::WriteAllText($extraPath, 'unexpected fixture')
    try {
        $null = Assert-ExactBundle
        throw 'Unexpected bundle member was accepted.'
    } catch {
        if ($_.Exception.Message -notlike 'Bundle membership mismatch.*') {
            throw
        }
    }
    Remove-Item -LiteralPath $extraPath -Force

    [IO.File]::WriteAllText($secondPath, 'changed fixture')
    try {
        $null = Assert-ExactBundle
        throw 'Changed bundle member was accepted.'
    } catch {
        if ($_.Exception.Message -notlike "Bundle hash mismatch for 'second.bin':*") {
            throw
        }
    }
} finally {
    foreach ($file in @(Get-ChildItem -LiteralPath $fixtureRoot -File -Force -ErrorAction SilentlyContinue)) {
        Remove-Item -LiteralPath $file.FullName -Force
    }
    Remove-Item -LiteralPath $fixtureRoot -Force
}

$probeOutputs = [ordered]@{
    lifecycle = 'tu WDDM Vulkan probe passed: Turnip WDDM, LUID-valid node mask 1, queue family 0'
    compute = 'tu WDDM Vulkan compute probe passed: Turnip WDDM, elements 256, checksum 3637120'
    graphics = 'tu WDDM Vulkan graphics probe passed: Turnip WDDM, 64x64 RGBA8, checksum 2088960'
}
$specs = @(Get-DirectProbeSpecs)
if (($specs.Name -join ',') -ne 'lifecycle,compute,graphics') {
    throw "Unexpected direct probe set: $($specs.Name -join ',')"
}
foreach ($spec in $specs) {
    $passing = Test-DirectProbeOutput $probeOutputs[$spec.Name] '' 0 $false $spec.Pattern
    if (-not $passing.Success -or -not $passing.MarkerPassed -or -not $passing.StderrEmpty) {
        throw "Passing marker fixture failed for '$($spec.Name)'."
    }
    $wrongMarker = Test-DirectProbeOutput ($probeOutputs[$spec.Name] + ' changed') '' 0 $false $spec.Pattern
    if ($wrongMarker.Success) {
        throw "Wrong marker fixture was accepted for '$($spec.Name)'."
    }
}
$nonemptyStderr = Test-DirectProbeOutput $probeOutputs.lifecycle 'diagnostic' 0 $false $specs[0].Pattern
if ($nonemptyStderr.Success -or $nonemptyStderr.StderrEmpty) {
    throw 'Nonempty direct-probe stderr was accepted.'
}
$timeout = Test-DirectProbeOutput $probeOutputs.compute '' 0 $true $specs[1].Pattern
if ($timeout.Success) {
    throw 'Timed-out direct probe was accepted.'
}

$destroyValueSuffixes = @(
    'Attempt', 'Status', 'Detail', 'HostResult', 'ContextId', 'ContextState',
    'OwnerState', 'Released', 'Retrying', 'OwnerRetained'
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
$cleanCheck = Test-DestroyDiagnostics $cleanDiagnostics
if (-not $cleanCheck.Passed -or $cleanCheck.SlotCount -ne 1 -or $cleanCheck.Errors.Count -ne 0) {
    throw "Clean destroy fixture failed: $($cleanCheck | ConvertTo-Json -Depth 6)"
}
$cleanDiagnostics.NativeContextDestroySlot02OwnerRetained = 1
$retainedCheck = Test-DestroyDiagnostics $cleanDiagnostics
if ($retainedCheck.Passed -or $retainedCheck.Errors.Count -ne 1) {
    throw "Retained-owner fixture was accepted: $($retainedCheck | ConvertTo-Json -Depth 6)"
}

$beforeDumps = [ordered]@{
    'C:\Windows\MEMORY.DMP' = [ordered]@{ Length = 1024; LastWriteTimeUtc = '2026-09-01T00:00:00.0000000Z' }
}
$sameDumps = [ordered]@{
    'C:\Windows\MEMORY.DMP' = [ordered]@{ Length = 1024; LastWriteTimeUtc = '2026-09-01T00:00:00.0000000Z' }
}
if (@(Get-NewOrChangedDumps $beforeDumps $sameDumps).Count -ne 0) {
    throw 'Unchanged dump fixture was reported as changed.'
}
$changedDumps = [ordered]@{
    'C:\Windows\MEMORY.DMP' = [ordered]@{ Length = 2048; LastWriteTimeUtc = '2026-09-01T00:01:00.0000000Z' }
    'C:\Windows\Minidump\090126-1.dmp' = [ordered]@{ Length = 512; LastWriteTimeUtc = '2026-09-01T00:01:00.0000000Z' }
}
$dumpChanges = @(Get-NewOrChangedDumps $beforeDumps $changedDumps)
if ($dumpChanges.Count -ne 2 -or $dumpChanges.Path -notcontains 'C:\Windows\MEMORY.DMP' -or
    $dumpChanges.Path -notcontains 'C:\Windows\Minidump\090126-1.dmp') {
    throw "Changed dump fixture failed: $($dumpChanges | ConvertTo-Json -Depth 6)"
}

$faultSince = [datetime]::SpecifyKind([datetime]'2026-09-01T08:20:47', [DateTimeKind]::Utc)
$faultUntil = $faultSince.AddMinutes(1)
$oldSystemEvent = [pscustomobject]@{
    TimeCreated = $faultSince.AddMinutes(-1)
    Id = 41
    ProviderName = 'fixture-system-old'
    LevelDisplayName = 'Critical'
    Message = 'old system event'
}
$newSystemEvent = [pscustomobject]@{
    TimeCreated = $faultSince.AddSeconds(1)
    Id = 41
    ProviderName = 'fixture-system-new'
    LevelDisplayName = 'Critical'
    Message = 'new system event'
}
$futureSystemEvent = [pscustomobject]@{
    TimeCreated = $faultUntil.AddMinutes(1)
    Id = 41
    ProviderName = 'fixture-system-future'
    LevelDisplayName = 'Critical'
    Message = 'future system event'
}
$oldApplicationEvent = [pscustomobject]@{
    TimeCreated = $faultSince.AddMinutes(-1)
    Id = 1000
    ProviderName = 'fixture-application-old'
    LevelDisplayName = 'Error'
    Message = 'old dwm.exe event'
}
$newApplicationEvent = [pscustomobject]@{
    TimeCreated = $faultSince.AddSeconds(1)
    Id = 1000
    ProviderName = 'fixture-application-new'
    LevelDisplayName = 'Error'
    Message = 'new dwm.exe event'
}
$futureApplicationEvent = [pscustomobject]@{
    TimeCreated = $faultUntil.AddMinutes(1)
    Id = 1000
    ProviderName = 'fixture-application-future'
    LevelDisplayName = 'Error'
    Message = 'future dwm.exe event'
}
function Get-WinEvent {
    param($FilterHashtable, $ErrorAction)

    if ($FilterHashtable.LogName -eq 'System') {
        @($oldSystemEvent, $newSystemEvent, $futureSystemEvent)
    } else {
        @($oldApplicationEvent, $newApplicationEvent, $futureApplicationEvent)
    }
}
try {
    $faultEvents = Get-FaultEvents -Since $faultSince -Until $faultUntil
    if ($faultEvents.Count -ne 2 -or
        $faultEvents.System.Count -ne 1 -or
        $faultEvents.Application.Count -ne 1 -or
        $faultEvents.System[0].ProviderName -ne 'fixture-system-new' -or
        $faultEvents.Application[0].ProviderName -ne 'fixture-application-new') {
        throw "Fault-event bounded-window fixture failed: $($faultEvents | ConvertTo-Json -Depth 6)"
    }
} finally {
    Remove-Item Function:\Get-WinEvent -ErrorAction SilentlyContinue
}

$argumentLine = Get-InteractiveArgumentLine 'C:\Program Files\runner.ps1' 'C:\Bundle Root' `
    'C:\Tools\capture.ps1' 'C:\Evidence Root'
$expectedArgumentLine = '-NoProfile -ExecutionPolicy Bypass -File "C:\Program Files\runner.ps1" ' +
    '-BundleRoot "C:\Bundle Root" -CaptureScriptPath "C:\Tools\capture.ps1" ' +
    '-OutputDirectory "C:\Evidence Root"'
if ($argumentLine -ne $expectedArgumentLine) {
    throw "Scheduled-task argument quoting fixture failed: $argumentLine"
}
try {
    $null = Get-InteractiveArgumentLine 'C:\bad"runner.ps1' 'C:\Bundle' 'C:\capture.ps1' 'C:\evidence'
    throw 'Quoted scheduled-task path was accepted.'
} catch {
    if ($_.Exception.Message -ne 'Scheduled-task paths must not contain double quotes.') {
        throw
    }
}

Write-Host "viogpu $ExpectedLabel Turnip wrapper fixtures: PASS"
