[CmdletBinding()]
param(
    [string]$PairRoot = 'C:\DroidVM\TurnipRuns\run-33457599974-coherence-pressure',
    [string]$BundleRoot = 'C:\DroidVM\TurnipRuns\run-33322445949\turnip-wddm-arm64-icd-bundle',
    [string]$OutputDirectory = 'C:\DroidVM\TurnipRuns\evidence\viogpu-58186-c913fd87-coherence-pressure',
    [ValidateRange(30, 900)]
    [int]$TimeoutSeconds = 300
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$expectedDriverVersion = '100.6.101.58186'
$expectedDriverHash = 'd3f920cb6a5367b468b831fd10d80308fd1272a5e2f32dc29a8a863442a91be2'
$expectedUmdHash = 'a65d1abeec1860fe9a8f8be58a53e6f71f8a635659dcbd42bb71b98b8a452754'
$expectedIcdHash = 'a1288acd874dd97c652a3b83316cf0aa69c5a2025c5a227371bcae98bb0216a5'
$expectedZlibHash = 'f8019e0161021feca20b765170c5b33a1cdc1e1bb393b858a90c294df8f71628'
$expectedPairHashes = [ordered]@{
    'tu_wddm_compute.spv' = '4718f697941f6fce807fc0e41cd5eb9a7c711b92d8549aef9ecef2a0369b5ba9'
    'tu_wddm_vulkan_compute_probe_arm64.exe' = '91bdcfaf6763ebbe1043ea0eff114de995d4527ac0b4353bb9f8e6583510abf7'
}
$pressureElements = 67108864
$pressureIterations = 16
$pressureBytes = 268435456
$pressureChecksum = 144115183613116416

function Get-GpuState {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
    })
    if ($devices.Count -ne 1) {
        throw "Expected one present virtio-gpu device, found $($devices.Count)."
    }
    $device = $devices[0]
    $property = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
    $driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($property.Data)"
    $driverKey = Get-Item -LiteralPath $driverKeyPath
    $serviceKey = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
    $imagePath = [Environment]::ExpandEnvironmentVariables([string]$serviceKey.GetValue('ImagePath', '')).Trim('"')
    if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
        $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
    }
    $image = Get-Item -LiteralPath $imagePath
    $umdPath = Join-Path $image.DirectoryName 'viogpud3d.dll'
    [pscustomobject]@{
        Status = [string]$device.Status
        ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) { $device.ProblemCode } else { $null }
        InstanceId = [string]$device.InstanceId
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        DriverImagePath = $image.FullName
        DriverImageSha256 = (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        UmdPath = $umdPath
        UmdSha256 = (Get-FileHash -LiteralPath $umdPath -Algorithm SHA256).Hash.ToLowerInvariant()
        DriverServiceStatus = [string](Get-Service -Name VioGpuWddm).Status
    }
}

function Test-GpuState {
    param(
        [object]$State,
        [string]$Label,
        [string]$ExpectedInstanceId = ''
    )

    $errors = [System.Collections.Generic.List[string]]::new()
    if ($State.Status -ne 'OK') {
        $errors.Add("${Label}: device status is '$($State.Status)', expected 'OK'.")
    }
    if ($null -ne $State.ProblemCode -and [int]$State.ProblemCode -ne 0) {
        $errors.Add("${Label}: problem code is '$($State.ProblemCode)', expected 0.")
    }
    if ($State.DriverVersion -ne $expectedDriverVersion) {
        $errors.Add("${Label}: driver version is '$($State.DriverVersion)', expected '$expectedDriverVersion'.")
    }
    if ($State.DriverImageSha256 -ne $expectedDriverHash) {
        $errors.Add("${Label}: SYS hash is '$($State.DriverImageSha256)', expected '$expectedDriverHash'.")
    }
    if ($State.UmdSha256 -ne $expectedUmdHash) {
        $errors.Add("${Label}: UMD hash is '$($State.UmdSha256)', expected '$expectedUmdHash'.")
    }
    if ($State.DriverServiceStatus -ne 'Running') {
        $errors.Add("${Label}: VioGpuWddm service is '$($State.DriverServiceStatus)', expected 'Running'.")
    }
    if (-not [string]::IsNullOrEmpty($ExpectedInstanceId) -and $State.InstanceId -ne $ExpectedInstanceId) {
        $errors.Add("${Label}: device instance changed from '$ExpectedInstanceId' to '$($State.InstanceId)'.")
    }
    [pscustomobject]@{
        Passed = $errors.Count -eq 0
        Errors = @($errors)
    }
}

function Measure-PressureDdiEvents {
    param([string[]]$Lines)

    $counts = [ordered]@{}
    foreach ($name in @('DdiBuildPagingBuffer', 'DdiPatch', 'DdiSubmitCommand')) {
        $eventLines = @($Lines | Where-Object { $_ -match ('"' + [regex]::Escape($name) + '\s+"') })
        $counts[$name] = [ordered]@{
            Start = @($eventLines | Where-Object { $_ -match ',\s*Start\s*,' }).Count
            Stop = @($eventLines | Where-Object { $_ -match ',\s*Stop\s*,' }).Count
            Total = $eventLines.Count
        }
    }
    $counts
}

function Test-CoherencePressureEvidence {
    param(
        [System.Collections.Specialized.OrderedDictionary]$Counts,
        [string]$Stdout,
        [string]$Stderr,
        [int]$ExitCode,
        [bool]$TimedOut
    )

    $errors = [System.Collections.Generic.List[string]]::new()
    foreach ($name in @('DdiBuildPagingBuffer', 'DdiPatch', 'DdiSubmitCommand')) {
        if (-not $Counts.Contains($name)) {
            $errors.Add("Trace counts are missing '$name'.")
            continue
        }
        if ($Counts[$name].Start -eq 0) {
            $errors.Add("Trace contains no $name start event.")
        }
        if ($Counts[$name].Start -ne $Counts[$name].Stop) {
            $errors.Add("Trace has unbalanced $name events: start=$($Counts[$name].Start) stop=$($Counts[$name].Stop).")
        }
    }

    $computePattern = '(?m)^tu WDDM Vulkan compute probe passed: .+, elements 67108864, checksum 144115183613116416\s*$'
    $pressurePattern = '(?m)^tu WDDM Vulkan coherence pressure passed: elements 67108864, iterations 16, bytes 268435456, host-cached 1, host-coherent [01]\s*$'
    $computeMatches = [regex]::Matches($Stdout, $computePattern).Count
    $pressureMatches = [regex]::Matches($Stdout, $pressurePattern).Count
    if ($TimedOut) {
        $errors.Add('The coherence-pressure probe timed out.')
    }
    if ($ExitCode -ne 0) {
        $errors.Add("The coherence-pressure probe exited with code $ExitCode.")
    }
    if ($computeMatches -ne 1) {
        $errors.Add("Expected one exact compute checksum marker, found $computeMatches.")
    }
    if ($pressureMatches -ne 1) {
        $errors.Add("Expected one exact coherence-pressure marker, found $pressureMatches.")
    }
    if (-not [string]::IsNullOrWhiteSpace($Stderr)) {
        $errors.Add('The coherence-pressure probe wrote to stderr.')
    }

    $ddiActivity = $Counts.Contains('DdiBuildPagingBuffer') -and
        $Counts.Contains('DdiPatch') -and $Counts.Contains('DdiSubmitCommand') -and
        $Counts.DdiBuildPagingBuffer.Start -gt 0 -and $Counts.DdiPatch.Start -gt 0 -and
        $Counts.DdiSubmitCommand.Start -gt 0
    [pscustomobject]@{
        Passed = $errors.Count -eq 0
        ExactLargeBufferCoherenceObserved = $computeMatches -eq 1 -and $pressureMatches -eq 1 -and
            -not $TimedOut -and $ExitCode -eq 0 -and [string]::IsNullOrWhiteSpace($Stderr)
        BalancedPagingDdiActivityObserved = $ddiActivity
        ForcedPageOutPageInProven = $false
        MultipassPrivateRecordRetentionProven = $false
        PartialPagingFailureRecoveryProven = $false
        Errors = @($errors)
    }
}

function Get-PressureArgumentLine {
    param([string]$ShaderPath)

    if ($ShaderPath.Contains('"')) {
        throw 'Shader path must not contain double quotes.'
    }
    "`"$ShaderPath`" --elements 67108864 --iterations 16"
}

function Assert-ExactFiles {
    param(
        [string]$Root,
        [System.Collections.Specialized.OrderedDictionary]$ExpectedHashes
    )

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Required directory does not exist: $Root"
    }
    $actualNames = @(Get-ChildItem -LiteralPath $Root -File | Sort-Object Name | ForEach-Object { $_.Name })
    $expectedNames = @($ExpectedHashes.Keys | Sort-Object)
    if (($actualNames -join "`n") -ne ($expectedNames -join "`n")) {
        throw "Pair membership mismatch. Expected $($expectedNames -join ', '); got $($actualNames -join ', ')."
    }
    foreach ($name in $expectedNames) {
        $actualHash = (Get-FileHash -LiteralPath (Join-Path $Root $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne $ExpectedHashes[$name]) {
            throw "$name SHA-256 mismatch: expected $($ExpectedHashes[$name]), got $actualHash"
        }
    }
}

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'The coherence-pressure observer must run elevated.'
}
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "Evidence directory already exists: $OutputDirectory"
}
Assert-ExactFiles $PairRoot $expectedPairHashes
$icdPath = Join-Path $BundleRoot 'vulkan_freedreno.dll'
$zlibPath = Join-Path $BundleRoot 'z-1.dll'
foreach ($required in @($icdPath, $zlibPath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required Turnip runtime file is missing: $required"
    }
}
$icdHash = (Get-FileHash -LiteralPath $icdPath -Algorithm SHA256).Hash.ToLowerInvariant()
$zlibHash = (Get-FileHash -LiteralPath $zlibPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($icdHash -ne $expectedIcdHash -or $zlibHash -ne $expectedZlibHash) {
    throw "Turnip runtime hash mismatch: ICD=$icdHash zlib=$zlibHash"
}

$initialState = Get-GpuState
$initialCheck = Test-GpuState $initialState 'initial'
if (-not $initialCheck.Passed) {
    throw "Unexpected initial GPU state: $($initialCheck.Errors -join ' ')"
}
$initialBoot = [datetime](Get-CimInstance Win32_OperatingSystem).LastBootUpTime
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$etlPath = Join-Path $OutputDirectory 'coherence-pressure.etl'
$csvPath = Join-Path $OutputDirectory 'coherence-pressure.csv'
$traceSummaryPath = Join-Path $OutputDirectory 'coherence-pressure-summary.xml'
$tracerptLogPath = Join-Path $OutputDirectory 'tracerpt.txt'
$stdoutPath = Join-Path $OutputDirectory 'pressure.stdout.txt'
$stderrPath = Join-Path $OutputDirectory 'pressure.stderr.txt'
$resultPath = Join-Path $OutputDirectory 'coherence-pressure-result.json'
$traceName = 'DroidVM-VioGpu-58186-CoherencePressure-' + (Get-Date -Format 'yyyyMMdd-HHmmssfff')
$probePath = Join-Path $PairRoot 'tu_wddm_vulkan_compute_probe_arm64.exe'
$shaderPath = Join-Path $PairRoot 'tu_wddm_compute.spv'
$traceStarted = $false
$child = $null
$childExitCode = $null
$timedOut = $false
$errors = [System.Collections.Generic.List[string]]::new()
$startedAt = Get-Date
$originalPath = $env:PATH

try {
    & logman.exe create trace $traceName -ow -o $etlPath -f bincirc -max 64 -bs 64 -nb 4 32 -ft 00:00:01 `
        -p Microsoft-Windows-DxgKrnl 0xFFFFFFFFFFFFFFFF 0xFF -ets | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "logman create failed with exit code $LASTEXITCODE."
    }
    $traceStarted = $true
    & logman.exe update $traceName -p '{D6B96B2C-72BF-4CA5-BB89-9FCA5C82F020}' `
        0x7FFFFFFF 0xFF -ets | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "logman provider update failed with exit code $LASTEXITCODE."
    }
    Start-Sleep -Seconds 2
    $env:PATH = "$BundleRoot;$originalPath"
    $argumentLine = Get-PressureArgumentLine $shaderPath
    $child = Start-Process -FilePath $probePath -WorkingDirectory $BundleRoot -ArgumentList $argumentLine `
        -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    $timedOut = -not $child.WaitForExit($TimeoutSeconds * 1000)
    if ($timedOut) {
        & taskkill.exe /PID $child.Id /T /F *> $null
        [void]$child.WaitForExit(5000)
        $childExitCode = 124
    } else {
        $child.WaitForExit()
        $child.Refresh()
        $childExitCode = [int]$child.ExitCode
    }
} catch {
    $errors.Add($_.Exception.Message)
} finally {
    $env:PATH = $originalPath
    if ($traceStarted) {
        & logman.exe stop $traceName -ets | Out-Null
        if ($LASTEXITCODE -ne 0) {
            $errors.Add("logman stop failed with exit code $LASTEXITCODE.")
        }
    }
}

$counts = [ordered]@{}
$evidence = $null
if (-not (Test-Path -LiteralPath $etlPath -PathType Leaf) -or (Get-Item -LiteralPath $etlPath).Length -eq 0) {
    $errors.Add('Coherence-pressure ETL is missing or empty.')
} else {
    $tracerptOutput = (& tracerpt.exe $etlPath -o $csvPath -of CSV -summary $traceSummaryPath -y 2>&1 | Out-String)
    [IO.File]::WriteAllText($tracerptLogPath, $tracerptOutput)
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $csvPath -PathType Leaf)) {
        $errors.Add("tracerpt failed with exit code $LASTEXITCODE.")
    } else {
        $counts = Measure-PressureDdiEvents @(Get-Content -LiteralPath $csvPath -Encoding UTF8)
    }
}
$stdout = if (Test-Path -LiteralPath $stdoutPath -PathType Leaf) {
    Get-Content -LiteralPath $stdoutPath -Raw
} else { '' }
$stderr = if (Test-Path -LiteralPath $stderrPath -PathType Leaf) {
    Get-Content -LiteralPath $stderrPath -Raw
} else { '' }
if ($null -eq $childExitCode) {
    $errors.Add('The coherence-pressure probe did not produce an exit code.')
} elseif ($counts.Count -ne 0) {
    $evidence = Test-CoherencePressureEvidence $counts $stdout $stderr $childExitCode $timedOut
    foreach ($message in $evidence.Errors) {
        $errors.Add($message)
    }
}

$finalState = Get-GpuState
$finalCheck = Test-GpuState $finalState 'final' $initialState.InstanceId
foreach ($message in $finalCheck.Errors) {
    $errors.Add($message)
}
$finalBoot = [datetime](Get-CimInstance Win32_OperatingSystem).LastBootUpTime
if ($finalBoot -ne $initialBoot) {
    $errors.Add('Windows rebooted during the coherence-pressure observer.')
}

$result = [ordered]@{
    Success = $errors.Count -eq 0
    StartedAt = $startedAt.ToString('o')
    CompletedAt = (Get-Date).ToString('o')
    PairRoot = $PairRoot
    PairHashes = $expectedPairHashes
    TurnipIcdPath = $icdPath
    TurnipIcdSha256 = $icdHash
    Elements = $pressureElements
    Iterations = $pressureIterations
    Bytes = $pressureBytes
    ExpectedChecksum = $pressureChecksum
    ChildProcessId = if ($null -eq $child) { $null } else { $child.Id }
    ChildTimedOut = $timedOut
    ChildExitCode = $childExitCode
    DdiCounts = $counts
    PressureEvidence = $evidence
    AcceptanceBoundary = [ordered]@{
        LargeHostCachedCoherence = 'exact 256 MiB, 16-submit GPU transform and CPU readback checksum'
        PagingDdiActivity = 'balanced generic DxgKrnl BuildPagingBuffer, Patch, and Submit boundaries in the same trace window'
        ForcedPageOutPageIn = 'not proven because this provider output does not expose the paging operation or process attribution'
        MultipassPrivateRecordRetention = 'not forced or proven by this observer'
        PartialPagingFailureRecovery = 'not injected or proven by this observer'
        AdapterWideTdrRestart = 'not triggered or proven by this observer'
    }
    InitialGpuState = $initialState
    InitialBootUpTime = $initialBoot.ToString('o')
    FinalGpuState = $finalState
    FinalGpuCheck = $finalCheck
    FinalBootUpTime = $finalBoot.ToString('o')
    EtlPath = $etlPath
    EtlSha256 = if (Test-Path -LiteralPath $etlPath -PathType Leaf) {
        (Get-FileHash -LiteralPath $etlPath -Algorithm SHA256).Hash.ToLowerInvariant()
    } else { $null }
    CsvPath = $csvPath
    TraceSummaryPath = $traceSummaryPath
    TracerptLogPath = $tracerptLogPath
    StdoutPath = $stdoutPath
    StderrPath = $stderrPath
    Errors = @($errors)
}
$json = $result | ConvertTo-Json -Depth 16
[IO.File]::WriteAllText($resultPath, $json)
$json
if (-not $result.Success) {
    exit 1
}
exit 0
