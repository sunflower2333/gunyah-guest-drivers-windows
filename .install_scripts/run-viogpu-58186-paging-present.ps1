[CmdletBinding()]
param(
    [string]$TurnipRunnerPath = 'C:\DroidVM\TurnipRuns\viogpu-58186-c913fd87\run-viogpu-58186-turnip.ps1',
    [string]$BundleRoot = 'C:\DroidVM\TurnipRuns\run-33322445949\turnip-wddm-arm64-icd-bundle',
    [string]$InteractiveRunnerPath = 'C:\DroidVM\TurnipRuns\viogpu-58186-c913fd87\run-viogpu-58186-win32-interactive.ps1',
    [string]$CaptureScriptPath = 'C:\DroidVM\TurnipRuns\viogpu-58186-c913fd87\capture-turnip-wddm-win32-window.ps1',
    [string]$OutputDirectory = 'C:\DroidVM\TurnipRuns\evidence\viogpu-58186-c913fd87-paging-present',
    [ValidateRange(300, 1800)]
    [int]$TimeoutSeconds = 900
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$expectedTurnipRunnerHash = 'f8f9e26ca5e5512b8c7d000d3515277e702babc05b84dfca5a650212fe299513'
$expectedDriverVersion = '100.6.101.58186'
$expectedDriverHash = 'd3f920cb6a5367b468b831fd10d80308fd1272a5e2f32dc29a8a863442a91be2'
$expectedUmdHash = 'a65d1abeec1860fe9a8f8be58a53e6f71f8a635659dcbd42bb71b98b8a452754'

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

function Measure-DdiEvents {
    param([string[]]$Lines)

    $names = @('DdiBuildPagingBuffer', 'DdiPatch', 'DdiSubmitCommand', 'DdiPresent')
    $counts = [ordered]@{}
    foreach ($name in $names) {
        $eventLines = @($Lines | Where-Object { $_ -match ('"' + [regex]::Escape($name) + '\s+"') })
        $counts[$name] = [ordered]@{
            Start = @($eventLines | Where-Object { $_ -match ',\s*Start\s*,' }).Count
            Stop = @($eventLines | Where-Object { $_ -match ',\s*Stop\s*,' }).Count
            Total = $eventLines.Count
        }
    }
    $counts
}

function Test-NormalPagingPresentEvidence {
    param(
        [System.Collections.Specialized.OrderedDictionary]$Counts,
        [object]$TurnipSummary
    )

    $errors = [System.Collections.Generic.List[string]]::new()
    foreach ($name in @('DdiBuildPagingBuffer', 'DdiPatch', 'DdiSubmitCommand', 'DdiPresent')) {
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

    $childPassed = $null -ne $TurnipSummary -and [bool]$TurnipSummary.Success
    if (-not $childPassed) {
        $errors.Add('The strict Turnip child suite did not pass.')
    }
    $probeResults = if ($null -eq $TurnipSummary) { @() } else { @($TurnipSummary.DirectProbeResults) }
    $compute = @($probeResults | Where-Object { $_.Name -eq 'compute' })
    $graphics = @($probeResults | Where-Object { $_.Name -eq 'graphics' })
    $coherenceObserved = $compute.Count -eq 1 -and $graphics.Count -eq 1 -and
        [bool]$compute[0].Success -and [bool]$graphics[0].Success
    if (-not $coherenceObserved) {
        $errors.Add('Compute and graphics checksum probes did not both pass.')
    }
    $interactivePassed = $null -ne $TurnipSummary -and $null -ne $TurnipSummary.InteractiveResult -and
        [bool]$TurnipSummary.InteractiveResult.Success
    if (-not $interactivePassed) {
        $errors.Add('The interactive Win32 probe and visible capture did not pass.')
    }

    [pscustomobject]@{
        Passed = $errors.Count -eq 0
        BuiltPagingToSubmitObserved = $Counts.DdiBuildPagingBuffer.Start -gt 0 -and
            $Counts.DdiPatch.Start -gt 0 -and $Counts.DdiSubmitCommand.Start -gt 0
        KmdPresentActivityObserved = $Counts.DdiPresent.Start -gt 0
        WorkloadChecksumCoherenceObserved = $coherenceObserved
        InteractiveVisiblePresentObserved = $interactivePassed
        MultipassPrivateRecordRetentionProven = $false
        PartialPagingFailureRecoveryProven = $false
        Errors = @($errors)
    }
}

function Get-ChildArgumentLine {
    param(
        [string]$RunnerPath,
        [string]$BundlePath,
        [string]$InteractivePath,
        [string]$CapturePath,
        [string]$EvidencePath
    )

    foreach ($value in @($RunnerPath, $BundlePath, $InteractivePath, $CapturePath, $EvidencePath)) {
        if ($value.Contains('"')) {
            throw 'Child paths must not contain double quotes.'
        }
    }
    "-NoProfile -ExecutionPolicy Bypass -File `"$RunnerPath`" " +
        "-BundleRoot `"$BundlePath`" -InteractiveRunnerPath `"$InteractivePath`" " +
        "-CaptureScriptPath `"$CapturePath`" -OutputDirectory `"$EvidencePath`""
}

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'The paging/Present observer must run elevated.'
}
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "Evidence directory already exists: $OutputDirectory"
}
if (-not (Test-Path -LiteralPath $TurnipRunnerPath -PathType Leaf)) {
    throw "Turnip runner does not exist: $TurnipRunnerPath"
}
$turnipRunnerHash = (Get-FileHash -LiteralPath $TurnipRunnerPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($turnipRunnerHash -ne $expectedTurnipRunnerHash) {
    throw "Turnip runner SHA-256 mismatch: expected $expectedTurnipRunnerHash, got $turnipRunnerHash"
}

$initialState = Get-GpuState
$initialCheck = Test-GpuState $initialState 'initial'
if (-not $initialCheck.Passed) {
    throw "Unexpected initial GPU state: $($initialCheck.Errors -join ' ')"
}
$initialBoot = [datetime](Get-CimInstance Win32_OperatingSystem).LastBootUpTime
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$childOutput = Join-Path $OutputDirectory 'turnip'
$etlPath = Join-Path $OutputDirectory 'paging-present.etl'
$csvPath = Join-Path $OutputDirectory 'paging-present.csv'
$traceSummaryPath = Join-Path $OutputDirectory 'paging-present-summary.xml'
$tracerptLogPath = Join-Path $OutputDirectory 'tracerpt.txt'
$childStdoutPath = Join-Path $OutputDirectory 'turnip-runner.stdout.txt'
$childStderrPath = Join-Path $OutputDirectory 'turnip-runner.stderr.txt'
$resultPath = Join-Path $OutputDirectory 'paging-present-result.json'
$traceName = 'DroidVM-VioGpu-58186-PagingPresent-' + (Get-Date -Format 'yyyyMMdd-HHmmssfff')
$traceStarted = $false
$child = $null
$childExitCode = $null
$timedOut = $false
$errors = [System.Collections.Generic.List[string]]::new()
$startedAt = Get-Date

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
    $argumentLine = Get-ChildArgumentLine $TurnipRunnerPath $BundleRoot $InteractiveRunnerPath `
        $CaptureScriptPath $childOutput
    $child = Start-Process -FilePath 'powershell.exe' -ArgumentList $argumentLine -PassThru `
        -RedirectStandardOutput $childStdoutPath -RedirectStandardError $childStderrPath
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
    if ($traceStarted) {
        & logman.exe stop $traceName -ets | Out-Null
        if ($LASTEXITCODE -ne 0) {
            $errors.Add("logman stop failed with exit code $LASTEXITCODE.")
        }
    }
}

$counts = [ordered]@{}
$turnipSummary = $null
$evidence = $null
if (-not (Test-Path -LiteralPath $etlPath -PathType Leaf) -or (Get-Item -LiteralPath $etlPath).Length -eq 0) {
    $errors.Add('Paging/Present ETL is missing or empty.')
} else {
    $tracerptOutput = (& tracerpt.exe $etlPath -o $csvPath -of CSV -summary $traceSummaryPath -y 2>&1 | Out-String)
    [IO.File]::WriteAllText($tracerptLogPath, $tracerptOutput)
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $csvPath -PathType Leaf)) {
        $errors.Add("tracerpt failed with exit code $LASTEXITCODE.")
    } else {
        $counts = Measure-DdiEvents @(Get-Content -LiteralPath $csvPath -Encoding UTF8)
    }
}
$childSummaryPath = Join-Path $childOutput 'turnip-summary.json'
if (-not (Test-Path -LiteralPath $childSummaryPath -PathType Leaf)) {
    $errors.Add('Turnip child summary is missing.')
} else {
    $turnipSummary = Get-Content -LiteralPath $childSummaryPath -Raw | ConvertFrom-Json
}
if ($childExitCode -ne 0) {
    $errors.Add("Turnip child exited with code $childExitCode.")
}
if ($counts.Count -ne 0 -and $null -ne $turnipSummary) {
    $evidence = Test-NormalPagingPresentEvidence $counts $turnipSummary
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
    $errors.Add('Windows rebooted during the paging/Present observer.')
}

$result = [ordered]@{
    Success = $errors.Count -eq 0
    StartedAt = $startedAt.ToString('o')
    CompletedAt = (Get-Date).ToString('o')
    TurnipRunnerPath = $TurnipRunnerPath
    TurnipRunnerSha256 = $turnipRunnerHash
    ChildProcessId = if ($null -eq $child) { $null } else { $child.Id }
    ChildTimedOut = $timedOut
    ChildExitCode = $childExitCode
    ChildSummaryPath = $childSummaryPath
    DdiCounts = $counts
    NormalPathEvidence = $evidence
    AcceptanceBoundary = [ordered]@{
        NormalBuiltPagingPatchSubmit = 'measured by balanced DxgKrnl DDI events and a passing Turnip suite'
        WorkloadCoherence = 'exact compute and graphics checksums; does not independently force a standard paging transfer'
        KmdPresentActivity = 'balanced DxgKrnl DdiPresent events plus interactive visible output; ETL is not process-attributed here'
        NormalWbStandardPagingCoherence = 'not independently forced or proven by this observer'
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
    Errors = @($errors)
}
$json = $result | ConvertTo-Json -Depth 16
[IO.File]::WriteAllText($resultPath, $json)
$json
if (-not $result.Success) {
    exit 1
}
exit 0
