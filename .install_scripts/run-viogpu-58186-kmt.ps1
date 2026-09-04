[CmdletBinding()]
param(
    [string]$ProbePath = 'C:\DroidVM\TurnipRuns\run-33339471439\turnip-wddm-arm64-icd-bundle\tu_wddm_kmt_probe_arm64.exe',
    [string]$OutputDirectory = 'C:\DroidVM\TurnipRuns\evidence\viogpu-58186-c913fd87-kmt',
    [int]$StressTimeoutMilliseconds = 180000,
    [int]$SubmitTimeoutMilliseconds = 30000
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$expectedProbeHash = '0311fc07741ef41306985234623f3e31a17eafdc577dbf41286cbb6e5383ca75'
$expectedDriverVersion = '100.6.101.58186'
$expectedDriverHash = 'd3f920cb6a5367b468b831fd10d80308fd1272a5e2f32dc29a8a863442a91be2'
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

function ConvertTo-U32 {
    param([object]$Value)

    [int64]$signed = [Convert]::ToInt64($Value)
    if ($signed -lt 0) {
        return [uint64]($signed + 0x100000000L)
    }
    return [uint64]$signed
}

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
    $service = Get-Service -Name VioGpuWddm

    [pscustomobject]@{
        Status = [string]$device.Status
        ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) { $device.ProblemCode } else { $null }
        InstanceId = [string]$device.InstanceId
        DriverKeyPath = $driverKeyPath
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        DriverImagePath = $imagePath
        DriverImageSha256 = (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()
        DriverServiceStatus = [string]$service.Status
    }
}

function Test-GpuState {
    param(
        [object]$State,
        [string]$Label,
        [string]$ExpectedInstanceId = ''
    )

    $stateErrors = [System.Collections.Generic.List[string]]::new()
    if ($null -eq $State) {
        $stateErrors.Add("${Label}: GPU state is unavailable.")
    } else {
        if ($State.Status -ne 'OK') {
            $stateErrors.Add("${Label}: device status is '$($State.Status)', expected 'OK'.")
        }
        if ($null -ne $State.ProblemCode -and [int]$State.ProblemCode -ne 0) {
            $stateErrors.Add("${Label}: problem code is $($State.ProblemCode), expected 0.")
        }
        if ($State.DriverVersion -ne $expectedDriverVersion) {
            $stateErrors.Add("${Label}: driver version is '$($State.DriverVersion)', expected '$expectedDriverVersion'.")
        }
        if ($State.DriverImageSha256 -ne $expectedDriverHash) {
            $stateErrors.Add("${Label}: SYS SHA-256 is '$($State.DriverImageSha256)', expected '$expectedDriverHash'.")
        }
        if ($State.DriverServiceStatus -ne 'Running') {
            $stateErrors.Add("${Label}: VioGpuWddm service is '$($State.DriverServiceStatus)', expected 'Running'.")
        }
        if (-not [string]::IsNullOrEmpty($ExpectedInstanceId) -and $State.InstanceId -ne $ExpectedInstanceId) {
            $stateErrors.Add("${Label}: device instance changed from '$ExpectedInstanceId' to '$($State.InstanceId)'.")
        }
    }

    [pscustomobject]@{
        Passed = $stateErrors.Count -eq 0
        Errors = @($stateErrors)
    }
}

function Get-NativeContextDiagnostics {
    param([string]$DriverKeyPath)

    $key = Get-Item -LiteralPath $DriverKeyPath
    $values = [ordered]@{}
    foreach ($name in @($key.GetValueNames() | Sort-Object)) {
        if ($name.StartsWith('NativeContext', [StringComparison]::Ordinal)) {
            $values[$name] = $key.GetValue(
                $name,
                $null,
                [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames
            )
        }
    }
    $values
}

function Remove-DestroyDiagnostics {
    param([string]$DriverKeyPath)

    $key = Get-Item -LiteralPath $DriverKeyPath
    foreach ($name in @($key.GetValueNames())) {
        if ($name.StartsWith('NativeContextDestroySlot', [StringComparison]::Ordinal)) {
            Remove-ItemProperty -LiteralPath $DriverKeyPath -Name $name -ErrorAction Stop
        }
    }
}

function Test-DestroyDiagnostics {
    param(
        [System.Collections.Specialized.OrderedDictionary]$Values,
        [string]$Label
    )

    $diagnosticErrors = [System.Collections.Generic.List[string]]::new()
    $slots = [System.Collections.Generic.List[object]]::new()
    $stageNames = @($Values.Keys | Where-Object {
        [string]$_ -match '^NativeContextDestroySlot[0-9]{2}Stage$'
    } | Sort-Object)
    if ($stageNames.Count -eq 0) {
        $diagnosticErrors.Add("${Label}: no committed Native Context destroy slot was recorded.")
    }

    foreach ($stageNameObject in $stageNames) {
        $stageName = [string]$stageNameObject
        if ($stageName -notmatch '^NativeContextDestroySlot([0-9]{2})Stage$') {
            continue
        }
        $slotNumber = [uint32]$Matches[1]
        $prefix = $stageName.Substring(0, $stageName.Length - 'Stage'.Length)
        $missing = @($destroyValueSuffixes | Where-Object { -not $Values.Contains($prefix + $_) })
        if ($missing.Count -ne 0) {
            $diagnosticErrors.Add("${Label}: slot $slotNumber is missing fields: $($missing -join ', ').")
            continue
        }

        $slot = [pscustomobject]@{
            Slot = $slotNumber
            Attempt = ConvertTo-U32 $Values[$prefix + 'Attempt']
            Stage = ConvertTo-U32 $Values[$prefix + 'Stage']
            Status = ConvertTo-U32 $Values[$prefix + 'Status']
            Detail = ConvertTo-U32 $Values[$prefix + 'Detail']
            HostResult = ConvertTo-U32 $Values[$prefix + 'HostResult']
            ContextId = ConvertTo-U32 $Values[$prefix + 'ContextId']
            ContextState = ConvertTo-U32 $Values[$prefix + 'ContextState']
            OwnerState = ConvertTo-U32 $Values[$prefix + 'OwnerState']
            Released = ConvertTo-U32 $Values[$prefix + 'Released']
            Retrying = ConvertTo-U32 $Values[$prefix + 'Retrying']
            OwnerRetained = ConvertTo-U32 $Values[$prefix + 'OwnerRetained']
        }
        $slots.Add($slot)

        if ($slot.Attempt -eq 0 -or
            $slot.Stage -ne 0x0FFF -or
            $slot.Status -ne 0 -or
            $slot.Detail -ne 0 -or
            $slot.HostResult -ne 1 -or
            $slot.ContextState -ne 4 -or
            $slot.OwnerState -ne 2 -or
            $slot.Released -ne 1 -or
            $slot.Retrying -ne 0 -or
            $slot.OwnerRetained -ne 0 -or
            ($slot.ContextId % 64) -ne $slot.Slot) {
            $diagnosticErrors.Add("${Label}: slot $slotNumber is not a clean terminal destroy record.")
        }
    }

    [pscustomobject]@{
        Passed = $diagnosticErrors.Count -eq 0
        SlotCount = $slots.Count
        Slots = @($slots)
        Errors = @($diagnosticErrors)
    }
}

function Test-StressMarkers {
    param([string]$Stdout, [string]$Stderr)

    $markerErrors = [System.Collections.Generic.List[string]]::new()
    foreach ($marker in @(
        'Stress lifecycle: passed iterations=10000',
        'Stress context lifecycle: passed iterations=10000',
        'Stress lifecycle summary: allocation_before=1 context_only=1 allocation_after=1',
        'tu WDDM KMT probe passed stage=allocation'
    )) {
        if ($Stdout -notmatch "(?m)^\s*$([regex]::Escape($marker))\s*$") {
            $markerErrors.Add("Lifecycle output is missing marker '$marker'.")
        }
    }
    if ($Stdout -match '(?m)^\s*Stress lifecycle: DestroyAllocation .* attempt=1 success=0\b') {
        $markerErrors.Add('Lifecycle output contains a round-1 allocation destroy failure.')
    }
    if ($Stdout -match '(?m)^\s*Stress (?:context )?lifecycle: failed\b') {
        $markerErrors.Add('Lifecycle output contains a stress failure marker.')
    }
    if (-not [string]::IsNullOrWhiteSpace($Stderr)) {
        $markerErrors.Add('Lifecycle stderr is not empty.')
    }

    [pscustomobject]@{
        Passed = $markerErrors.Count -eq 0
        Errors = @($markerErrors)
    }
}

function Test-SubmitMarkers {
    param([string]$Stdout, [string]$Stderr)

    $markerErrors = [System.Collections.Generic.List[string]]::new()
    $render = [regex]::Match($Stdout, '(?m)^\s*Submit probe Render\(NOP\): success=1 fence=([0-9]+)\s*$')
    $fence = [regex]::Match($Stdout, '(?m)^\s*Submit probe fence: completed=1 query=1 value=([0-9]+)\s*$')
    if (-not $render.Success) {
        $markerErrors.Add('Submit output is missing the successful Render(NOP) marker.')
    }
    if (-not $fence.Success) {
        $markerErrors.Add('Submit output is missing the completed/queryable fence marker.')
    }
    if ($render.Success -and $fence.Success -and [uint64]$render.Groups[1].Value -ne [uint64]$fence.Groups[1].Value) {
        $markerErrors.Add("Submit fence '$($render.Groups[1].Value)' does not match completed fence '$($fence.Groups[1].Value)'.")
    }
    if ($Stdout -notmatch '(?m)^\s*tu WDDM KMT probe passed stage=allocation\s*$') {
        $markerErrors.Add('Submit output is missing the allocation-stage pass marker.')
    }
    if (-not [string]::IsNullOrWhiteSpace($Stderr)) {
        $markerErrors.Add('Submit stderr is not empty.')
    }

    [pscustomobject]@{
        Passed = $markerErrors.Count -eq 0
        SubmittedFence = if ($render.Success) { [uint64]$render.Groups[1].Value } else { $null }
        CompletedFence = if ($fence.Success) { [uint64]$fence.Groups[1].Value } else { $null }
        Errors = @($markerErrors)
    }
}

function Invoke-KmtPhase {
    param(
        [string]$Name,
        [string[]]$ProbeArguments,
        [int]$TimeoutMilliseconds
    )

    $phaseErrors = [System.Collections.Generic.List[string]]::new()
    $phaseDirectory = Join-Path $OutputDirectory $Name
    New-Item -ItemType Directory -Path $phaseDirectory | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmssfff'
    $traceName = "DroidVM-VioGpu-58186-$Name-$stamp"
    $etlPath = Join-Path $phaseDirectory "$Name-$stamp.etl"
    $csvPath = Join-Path $phaseDirectory "$Name-$stamp.csv"
    $traceSummaryPath = Join-Path $phaseDirectory "$Name-$stamp-summary.xml"
    $tracerptLogPath = Join-Path $phaseDirectory "$Name-$stamp-tracerpt.txt"
    $stdoutPath = Join-Path $phaseDirectory "$Name-$stamp.stdout.txt"
    $stderrPath = Join-Path $phaseDirectory "$Name-$stamp.stderr.txt"
    $resultPath = Join-Path $phaseDirectory "$Name-$stamp.result.json"
    $traceStarted = $false
    $traceStopExitCode = $null
    $probeProcessId = $null
    $probeExitCode = $null
    $timedOut = $false
    $startedAt = Get-Date
    $before = $null
    $after = $null
    $diagnosticsBefore = [ordered]@{}
    $diagnosticsAfter = [ordered]@{}

    try {
        $before = Get-GpuState
        $beforeCheck = Test-GpuState $before "$Name before"
        foreach ($message in $beforeCheck.Errors) {
            $phaseErrors.Add($message)
        }
        if (-not $beforeCheck.Passed) {
            throw "$Name preflight GPU state failed."
        }

        $diagnosticsBefore = Get-NativeContextDiagnostics $before.DriverKeyPath
        Remove-DestroyDiagnostics $before.DriverKeyPath

        & logman.exe create trace $traceName -ow -o $etlPath -f bincirc -max 32 -bs 64 -nb 2 8 -ft 00:00:01 `
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
        $probe = Start-Process -FilePath $ProbePath -WorkingDirectory (Split-Path $ProbePath) `
            -ArgumentList $ProbeArguments -PassThru -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath
        $probeProcessId = $probe.Id
        $timedOut = -not $probe.WaitForExit($TimeoutMilliseconds)
        if ($timedOut) {
            & taskkill.exe /PID $probe.Id /T /F *> $null
            if (-not $probe.WaitForExit(5000)) {
                Stop-Process -Id $probe.Id -Force -ErrorAction SilentlyContinue
            }
            $probeExitCode = 124
        } else {
            $probe.WaitForExit()
            $probe.Refresh()
            $probeExitCode = [int]$probe.ExitCode
        }
    } catch {
        $phaseErrors.Add($_.Exception.Message)
    } finally {
        if ($traceStarted) {
            & logman.exe stop $traceName -ets | Out-Null
            $traceStopExitCode = $LASTEXITCODE
            if ($traceStopExitCode -ne 0) {
                $phaseErrors.Add("logman stop failed with exit code $traceStopExitCode.")
            }
        }
    }

    try {
        $after = Get-GpuState
        $expectedInstanceId = if ($null -eq $before) { '' } else { [string]$before.InstanceId }
        $afterCheck = Test-GpuState $after "$Name after" $expectedInstanceId
        foreach ($message in $afterCheck.Errors) {
            $phaseErrors.Add($message)
        }
        $diagnosticsAfter = Get-NativeContextDiagnostics $after.DriverKeyPath
    } catch {
        $phaseErrors.Add("${Name} postflight collection failed: $($_.Exception.Message)")
    }

    $stdout = if (Test-Path -LiteralPath $stdoutPath -PathType Leaf) { [IO.File]::ReadAllText($stdoutPath) } else { '' }
    $stderr = if (Test-Path -LiteralPath $stderrPath -PathType Leaf) { [IO.File]::ReadAllText($stderrPath) } else { '' }
    if ($probeExitCode -ne 0) {
        $phaseErrors.Add("$Name probe exit code is '$probeExitCode', expected 0.")
    }
    if ($timedOut) {
        $phaseErrors.Add("$Name probe exceeded its ${TimeoutMilliseconds}ms timeout.")
    }

    $markerCheck = if ($Name -eq 'lifecycle') {
        Test-StressMarkers $stdout $stderr
    } else {
        Test-SubmitMarkers $stdout $stderr
    }
    foreach ($message in $markerCheck.Errors) {
        $phaseErrors.Add($message)
    }

    $destroyDiagnostics = Test-DestroyDiagnostics $diagnosticsAfter $Name
    foreach ($message in $destroyDiagnostics.Errors) {
        $phaseErrors.Add($message)
    }

    $etlLength = if (Test-Path -LiteralPath $etlPath -PathType Leaf) { (Get-Item -LiteralPath $etlPath).Length } else { 0 }
    $etlSha256 = $null
    $tracerptExitCode = $null
    $csvLength = 0
    $traceSummaryLength = 0
    if ($etlLength -le 0) {
        $phaseErrors.Add("$Name ETL evidence is missing or empty.")
    } else {
        $etlSha256 = (Get-FileHash -LiteralPath $etlPath -Algorithm SHA256).Hash.ToLowerInvariant()
        $tracerptOutput = (& tracerpt.exe $etlPath -o $csvPath -of CSV -summary $traceSummaryPath -y 2>&1 | Out-String)
        $tracerptExitCode = $LASTEXITCODE
        [IO.File]::WriteAllText($tracerptLogPath, $tracerptOutput)
        $csvLength = if (Test-Path -LiteralPath $csvPath -PathType Leaf) { (Get-Item -LiteralPath $csvPath).Length } else { 0 }
        $traceSummaryLength = if (Test-Path -LiteralPath $traceSummaryPath -PathType Leaf) {
            (Get-Item -LiteralPath $traceSummaryPath).Length
        } else { 0 }
        if ($tracerptExitCode -ne 0 -or $csvLength -le 0 -or $traceSummaryLength -le 0) {
            $phaseErrors.Add("$Name ETL parse failed: tracerpt=$tracerptExitCode csv=$csvLength summary=$traceSummaryLength.")
        }
    }

    $result = [ordered]@{
        Success = $phaseErrors.Count -eq 0
        StartedAt = $startedAt.ToString('o')
        CompletedAt = (Get-Date).ToString('o')
        Name = $Name
        ProbePath = $ProbePath
        ProbeSha256 = $expectedProbeHash
        ProbeArguments = $ProbeArguments
        ProbeProcessId = $probeProcessId
        ProbeTimedOut = $timedOut
        ProbeExitCode = $probeExitCode
        StdoutPath = $stdoutPath
        StderrPath = $stderrPath
        MarkerCheck = $markerCheck
        EtlPath = $etlPath
        EtlLength = $etlLength
        EtlSha256 = $etlSha256
        TraceStopExitCode = $traceStopExitCode
        TracerptExitCode = $tracerptExitCode
        CsvPath = $csvPath
        CsvLength = $csvLength
        TraceSummaryPath = $traceSummaryPath
        TraceSummaryLength = $traceSummaryLength
        TracerptLogPath = $tracerptLogPath
        DiagnosticsBefore = $diagnosticsBefore
        DiagnosticsAfter = $diagnosticsAfter
        DestroyDiagnostics = $destroyDiagnostics
        GpuBefore = $before
        GpuAfter = $after
        Errors = @($phaseErrors)
        ResultPath = $resultPath
    }
    [IO.File]::WriteAllText($resultPath, ($result | ConvertTo-Json -Depth 12))
    [pscustomobject]$result
}

if (-not (Test-Path -LiteralPath $ProbePath -PathType Leaf)) {
    throw "KMT probe does not exist: $ProbePath"
}
$probeHash = (Get-FileHash -LiteralPath $ProbePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($probeHash -ne $expectedProbeHash) {
    throw "KMT probe SHA-256 mismatch: expected $expectedProbeHash, got $probeHash"
}
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "Evidence directory already exists: $OutputDirectory"
}

$initialState = Get-GpuState
$initialCheck = Test-GpuState $initialState 'initial'
if (-not $initialCheck.Passed) {
    throw "Unexpected initial GPU state: $($initialCheck.Errors -join ' ')"
}

New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$lifecycle = Invoke-KmtPhase 'lifecycle' @('--stage=allocation', '--stress-lifecycle') $StressTimeoutMilliseconds
$submit = $null
if ($lifecycle.Success) {
    $submit = Invoke-KmtPhase 'submit-nop' @('--stage=allocation', '--submit-nop') $SubmitTimeoutMilliseconds
}

$finalState = $null
$finalCheck = $null
try {
    $finalState = Get-GpuState
    $finalCheck = Test-GpuState $finalState 'final' $initialState.InstanceId
} catch {
    $finalCheck = [pscustomobject]@{
        Passed = $false
        Errors = @("Final GPU state collection failed: $($_.Exception.Message)")
    }
}
$success = $lifecycle.Success -and $null -ne $submit -and $submit.Success -and $finalCheck.Passed
$summaryPath = Join-Path $OutputDirectory 'kmt-summary.json'
$summary = [ordered]@{
    Success = $success
    CompletedAt = (Get-Date).ToString('o')
    ExpectedDriverVersion = $expectedDriverVersion
    ExpectedDriverSha256 = $expectedDriverHash
    ProbePath = $ProbePath
    ProbeSha256 = $probeHash
    InitialGpuState = $initialState
    LifecycleResultPath = $lifecycle.ResultPath
    LifecycleSuccess = $lifecycle.Success
    SubmitResultPath = if ($null -eq $submit) { $null } else { $submit.ResultPath }
    SubmitSuccess = if ($null -eq $submit) { $false } else { $submit.Success }
    FinalGpuState = $finalState
    FinalGpuCheck = $finalCheck
}
$json = $summary | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText($summaryPath, $json)
$json
if (-not $success) {
    exit 1
}
exit 0
