[CmdletBinding()]
param(
    [string]$BundleRoot = 'C:\DroidVM\TurnipRuns\run-33322445949\turnip-wddm-arm64-icd-bundle',
    [string]$InteractiveRunnerPath = 'C:\Users\USER\viogpu-58188-d7fe1b39\run-viogpu-58188-win32-interactive.ps1',
    [string]$CaptureScriptPath = 'C:\DroidVM\TurnipRuns\viogpu-58186-c913fd87\capture-turnip-wddm-win32-window.ps1',
    [string]$OutputDirectory = 'C:\DroidVM\TurnipRuns\evidence\viogpu-58188-d7fe1b39-turnip',
    [string]$TaskName = 'DroidVM-Turnip-58188-Interactive',
    [string]$InteractiveUserId = 'DROIDVM\USER',
    [ValidateRange(10, 300)]
    [int]$DirectProbeTimeoutSeconds = 120,
    [ValidateRange(30, 900)]
    [int]$InteractiveTimeoutSeconds = 300,
    [ValidatePattern('^\d+\.\d+\.\d+\.\d+$')]
    [string]$DriverVersion = '100.6.101.58188',
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string]$DriverImageSha256 = 'b699bb08b055034fe72b1e6676993fb0d0152f663330fa9c34db0820231d40ca',
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string]$UmdImageSha256 = 'd89f3d930dfe4d0eb3480f3b7495243e7c6805eb3d94d11fc497e7c180345474',
    [switch]$SkipInteractive
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$expectedDriverVersion = $DriverVersion
$expectedDriverHash = $DriverImageSha256.ToLowerInvariant()
$expectedUmdHash = $UmdImageSha256.ToLowerInvariant()
$expectedInteractiveRunnerHash = '6255e0626028f2fce425a49c3b6786c9e8f667dccc403e92675d1c8326026906'
$expectedCaptureScriptHash = '560da1a81144ffc7d6432b01378d21baa7cdf962d69906ab2d96744925c5f77e'
$expectedBundleHashes = [ordered]@{
    'SHA256SUMS.txt' = '46a9de5aef11199c6caa8ad02c85fa68f602334ebb286de35e6bdf6c0d3b2762'
    'TURNIP_WDDM_ICD.md' = 'caca97a6fd08d1233cc81f0091c1c3dbaa51bb31cf8c2b8d41d0ebecf3751df0'
    'freedreno_icd.arm64.json' = '4344a882ad2aa543f69c8a48d152b6f3983a8349aa8a95b1c35d884989c53bb1'
    'tu_wddm_compute.spv' = '10e42787bf8e32aa36893d46c5b3d7bcabfd997e4c799116f383ff3edfc5eadf'
    'tu_wddm_graphics.frag.spv' = '803d688da968ebc017887009ec4f1234a323e8d3aaed1d1c8f6e9d0c4ca1af05'
    'tu_wddm_graphics.vert.spv' = 'e37d0dca1f6fccefef89a4b49735ed6e99e4997d806d7c5e8b4a8f2945d83646'
    'tu_wddm_kmt_probe_arm64.exe' = '5730591e852d46f360927f93889323e629b1bc13c1cbf9b6ecf9f97b7898e77b'
    'tu_wddm_vulkan_compute_probe_arm64.exe' = '62dd0a0fef2313ec12ce0036135d7acbf428a84fb3765a85445d909d15e103a5'
    'tu_wddm_vulkan_graphics_probe_arm64.exe' = '8d72e463e9b5b738fce446235691782e0e7b488a842de49a423090f5a2314081'
    'tu_wddm_vulkan_probe_arm64.exe' = '3315b4d4f765571838320eb7a276ed686e806ef0c7cc30f0336c88966f4b1b89'
    'tu_wddm_win32_probe_arm64.exe' = '848b4ae08c97d9d3d8db1408f04b865e192e8df014fd92e5f557b631184271b4'
    'turnip-wddm-icd.ps1' = 'd181e0b2b2bc10e21bae28d4363cf0ae61338ddc59e618ffcabb1ab3873603e9'
    'vulkan_freedreno.dll' = 'a1288acd874dd97c652a3b83316cf0aa69c5a2025c5a227371bcae98bb0216a5'
    'z-1.dll' = 'f8019e0161021feca20b765170c5b33a1cdc1e1bb393b858a90c294df8f71628'
}
$destroyValueSuffixes = @(
    'Attempt', 'Status', 'Detail', 'HostResult', 'ContextId', 'ContextState',
    'OwnerState', 'Released', 'Retrying', 'OwnerRetained'
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
    $image = Get-Item -LiteralPath $imagePath
    $umdPath = Join-Path $image.DirectoryName 'viogpud3d.dll'
    [pscustomobject]@{
        Status = [string]$device.Status
        ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) { $device.ProblemCode } else { $null }
        InstanceId = [string]$device.InstanceId
        DriverKeyPath = $driverKeyPath
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        InfPath = [string]$driverKey.GetValue('InfPath', '')
        DriverImagePath = $image.FullName
        DriverImageSha256 = (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        UmdPath = $umdPath
        UmdSha256 = (Get-FileHash -LiteralPath $umdPath -Algorithm SHA256).Hash.ToLowerInvariant()
        DriverSignatureStatus = [string](Get-AuthenticodeSignature -LiteralPath $image.FullName).Status
        UmdSignatureStatus = [string](Get-AuthenticodeSignature -LiteralPath $umdPath).Status
        DriverServiceStatus = [string](Get-Service -Name VioGpuWddm).Status
    }
}

function Test-GpuState {
    param([object]$State, [string]$Label, [string]$ExpectedInstanceId = '')

    $stateErrors = [System.Collections.Generic.List[string]]::new()
    if ($State.Status -ne 'OK') {
        $stateErrors.Add("${Label}: device status is '$($State.Status)', expected 'OK'.")
    }
    if ($null -ne $State.ProblemCode -and [int]$State.ProblemCode -ne 0) {
        $stateErrors.Add("${Label}: problem code is '$($State.ProblemCode)', expected 0.")
    }
    if ($State.DriverVersion -ne $expectedDriverVersion) {
        $stateErrors.Add("${Label}: driver version is '$($State.DriverVersion)', expected '$expectedDriverVersion'.")
    }
    if ($State.DriverImageSha256 -ne $expectedDriverHash) {
        $stateErrors.Add("${Label}: SYS hash is '$($State.DriverImageSha256)', expected '$expectedDriverHash'.")
    }
    if ($State.UmdSha256 -ne $expectedUmdHash) {
        $stateErrors.Add("${Label}: UMD hash is '$($State.UmdSha256)', expected '$expectedUmdHash'.")
    }
    if ($State.DriverSignatureStatus -ne 'Valid' -or $State.UmdSignatureStatus -ne 'Valid') {
        $stateErrors.Add("${Label}: installed SYS/UMD signatures are not both Valid.")
    }
    if ($State.DriverServiceStatus -ne 'Running') {
        $stateErrors.Add("${Label}: VioGpuWddm service is '$($State.DriverServiceStatus)', expected 'Running'.")
    }
    if (-not [string]::IsNullOrEmpty($ExpectedInstanceId) -and $State.InstanceId -ne $ExpectedInstanceId) {
        $stateErrors.Add("${Label}: device instance changed from '$ExpectedInstanceId' to '$($State.InstanceId)'.")
    }
    [pscustomobject]@{
        Passed = $stateErrors.Count -eq 0
        Errors = @($stateErrors)
    }
}

function Assert-ExactBundle {
    $items = @(Get-ChildItem -LiteralPath $BundleRoot -Force)
    $unexpectedContainers = @($items | Where-Object { $_.PSIsContainer })
    if ($unexpectedContainers.Count -ne 0) {
        throw "Bundle contains unexpected directories: $($unexpectedContainers.Name -join ', ')"
    }
    $actualNames = @($items.Name | Sort-Object)
    $expectedNames = @($expectedBundleHashes.Keys | Sort-Object)
    if (($actualNames -join "`n") -ne ($expectedNames -join "`n")) {
        throw "Bundle membership mismatch. Expected $($expectedNames -join ', '); got $($actualNames -join ', ')."
    }
    foreach ($name in $expectedBundleHashes.Keys) {
        $path = Join-Path $BundleRoot $name
        $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne $expectedBundleHashes[$name]) {
            throw "Bundle hash mismatch for '$name': expected $($expectedBundleHashes[$name]), got $actualHash"
        }
    }
    [pscustomobject]@{
        EntryCount = $expectedNames.Count
        Entries = $expectedNames
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
    param([System.Collections.Specialized.OrderedDictionary]$Values)

    $diagnosticErrors = [System.Collections.Generic.List[string]]::new()
    $slots = [System.Collections.Generic.List[object]]::new()
    $stageNames = @($Values.Keys | Where-Object {
        [string]$_ -match '^NativeContextDestroySlot[0-9]{2}Stage$'
    } | Sort-Object)
    if ($stageNames.Count -eq 0) {
        $diagnosticErrors.Add('No committed Native Context destroy slot was recorded by the Turnip suite.')
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
            $diagnosticErrors.Add("Slot $slotNumber is missing fields: $($missing -join ', ').")
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
        if ($slot.Attempt -eq 0 -or $slot.Stage -ne 0x0FFF -or $slot.Status -ne 0 -or
            $slot.Detail -ne 0 -or $slot.HostResult -ne 1 -or $slot.ContextState -ne 4 -or
            $slot.OwnerState -ne 2 -or $slot.Released -ne 1 -or $slot.Retrying -ne 0 -or
            $slot.OwnerRetained -ne 0 -or ($slot.ContextId % 64) -ne $slot.Slot) {
            $diagnosticErrors.Add("Slot $slotNumber is not a clean terminal destroy record.")
        }
    }
    [pscustomobject]@{
        Passed = $diagnosticErrors.Count -eq 0
        SlotCount = $slots.Count
        Slots = @($slots)
        Errors = @($diagnosticErrors)
    }
}

function Get-DumpSnapshot {
    $paths = [System.Collections.Generic.List[string]]::new()
    if (Test-Path -LiteralPath 'C:\Windows\MEMORY.DMP' -PathType Leaf) {
        $paths.Add('C:\Windows\MEMORY.DMP')
    }
    foreach ($file in @(Get-ChildItem -LiteralPath 'C:\Windows\Minidump' -File -ErrorAction SilentlyContinue)) {
        $paths.Add($file.FullName)
    }
    $snapshot = [ordered]@{}
    foreach ($path in @($paths | Sort-Object)) {
        $item = Get-Item -LiteralPath $path
        $snapshot[$item.FullName] = [ordered]@{
            Length = $item.Length
            LastWriteTimeUtc = $item.LastWriteTimeUtc.ToString('o')
        }
    }
    $snapshot
}

function Get-NewOrChangedDumps {
    param(
        [System.Collections.Specialized.OrderedDictionary]$Before,
        [System.Collections.Specialized.OrderedDictionary]$After
    )

    $changed = [System.Collections.Generic.List[object]]::new()
    foreach ($path in $After.Keys) {
        if (-not $Before.Contains($path) -or
            $Before[$path].Length -ne $After[$path].Length -or
            $Before[$path].LastWriteTimeUtc -ne $After[$path].LastWriteTimeUtc) {
            $changed.Add([pscustomobject]@{
                Path = $path
                Before = if ($Before.Contains($path)) { $Before[$path] } else { $null }
                After = $After[$path]
            })
        }
    }
    @($changed)
}

function Get-FaultEvents {
    param(
        [datetime]$Since,
        [datetime]$Until
    )

    $sinceUtc = $Since.ToUniversalTime()
    $untilUtc = $Until.ToUniversalTime()
    if ($untilUtc -lt $sinceUtc) {
        throw 'Fault-event collection end precedes its start.'
    }
    $systemEvents = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $Since; EndTime = $Until } `
        -ErrorAction SilentlyContinue | Where-Object {
            $null -ne $_.TimeCreated -and
            $_.TimeCreated.ToUniversalTime() -ge $sinceUtc -and
            $_.TimeCreated.ToUniversalTime() -le $untilUtc -and
            $_.Id -in @(14, 17, 18, 19, 41, 1001, 4101, 6008)
        } | Select-Object TimeCreated, Id, ProviderName, LevelDisplayName, Message)
    $applicationEvents = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $Since; EndTime = $Until } `
        -ErrorAction SilentlyContinue | Where-Object {
            $null -ne $_.TimeCreated -and
            $_.TimeCreated.ToUniversalTime() -ge $sinceUtc -and
            $_.TimeCreated.ToUniversalTime() -le $untilUtc -and
            $_.Id -in @(1000, 1001) -and
            $_.Message -match '(?i)tu_wddm|vulkan_freedreno|viogpu|dwm\.exe|dwmcore\.dll'
        } | Select-Object TimeCreated, Id, ProviderName, LevelDisplayName, Message)
    [pscustomobject]@{
        System = $systemEvents
        Application = $applicationEvents
        Count = $systemEvents.Count + $applicationEvents.Count
    }
}

function Get-DirectProbeSpecs {
    @(
        [pscustomobject]@{
            Name = 'lifecycle'
            Executable = 'tu_wddm_vulkan_probe_arm64.exe'
            Arguments = @()
            Pattern = '(?m)^tu WDDM Vulkan probe passed: .+, LUID-valid node mask 1, queue family [0-9]+\s*$'
        },
        [pscustomobject]@{
            Name = 'compute'
            Executable = 'tu_wddm_vulkan_compute_probe_arm64.exe'
            Arguments = @('tu_wddm_compute.spv')
            Pattern = '(?m)^tu WDDM Vulkan compute probe passed: .+, elements 256, checksum 3637120\s*$'
        },
        [pscustomobject]@{
            Name = 'graphics'
            Executable = 'tu_wddm_vulkan_graphics_probe_arm64.exe'
            Arguments = @('tu_wddm_graphics.vert.spv', 'tu_wddm_graphics.frag.spv')
            Pattern = '(?m)^tu WDDM Vulkan graphics probe passed: .+, 64x64 RGBA8, checksum 2088960\s*$'
        }
    )
}

function Test-DirectProbeOutput {
    param(
        [string]$Stdout,
        [string]$Stderr,
        [int]$ExitCode,
        [bool]$TimedOut,
        [string]$PassPattern
    )

    $markerPassed = $Stdout -match $PassPattern
    $stderrEmpty = [string]::IsNullOrWhiteSpace($Stderr)
    [pscustomobject]@{
        Success = -not $TimedOut -and $ExitCode -eq 0 -and $markerPassed -and $stderrEmpty
        MarkerPassed = $markerPassed
        StderrEmpty = $stderrEmpty
    }
}

function Invoke-DirectProbe {
    param(
        [string]$Name,
        [string]$ExecutableName,
        [string[]]$ArgumentNames,
        [string]$PassPattern
    )

    $phaseDirectory = Join-Path $OutputDirectory $Name
    New-Item -ItemType Directory -Path $phaseDirectory | Out-Null
    $stdoutPath = Join-Path $phaseDirectory "$Name.stdout.txt"
    $stderrPath = Join-Path $phaseDirectory "$Name.stderr.txt"
    $resultPath = Join-Path $phaseDirectory "$Name.result.json"
    $executable = Join-Path $BundleRoot $ExecutableName
    $arguments = @($ArgumentNames | ForEach-Object { Join-Path $BundleRoot $_ })
    $startedAt = Get-Date
    $startProcessParameters = @{
        FilePath = $executable
        WorkingDirectory = $BundleRoot
        PassThru = $true
        RedirectStandardOutput = $stdoutPath
        RedirectStandardError = $stderrPath
    }
    if ($arguments.Count -ne 0) {
        $startProcessParameters.ArgumentList = $arguments
    }
    $process = Start-Process @startProcessParameters
    $timedOut = -not $process.WaitForExit($DirectProbeTimeoutSeconds * 1000)
    if ($timedOut) {
        & taskkill.exe /PID $process.Id /T /F *> $null
        if (-not $process.WaitForExit(5000)) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
        $exitCode = 124
    } else {
        $process.WaitForExit()
        $process.Refresh()
        $exitCode = [int]$process.ExitCode
    }
    $stdout = if (Test-Path -LiteralPath $stdoutPath) { [IO.File]::ReadAllText($stdoutPath) } else { '' }
    $stderr = if (Test-Path -LiteralPath $stderrPath) { [IO.File]::ReadAllText($stderrPath) } else { '' }
    $assessment = Test-DirectProbeOutput $stdout $stderr $exitCode $timedOut $PassPattern
    $result = [ordered]@{
        Success = $assessment.Success
        Name = $Name
        StartedAt = $startedAt.ToString('o')
        CompletedAt = (Get-Date).ToString('o')
        Executable = $executable
        ExecutableSha256 = $expectedBundleHashes[$ExecutableName]
        Arguments = $arguments
        ProcessId = $process.Id
        TimedOut = $timedOut
        ExitCode = $exitCode
        MarkerPassed = $assessment.MarkerPassed
        StderrEmpty = $assessment.StderrEmpty
        StdoutPath = $stdoutPath
        StderrPath = $stderrPath
        Stdout = $stdout
        Stderr = $stderr
        ResultPath = $resultPath
    }
    [IO.File]::WriteAllText($resultPath, ($result | ConvertTo-Json -Depth 7))
    [pscustomobject]$result
}

function Get-InteractiveArgumentLine {
    param(
        [string]$RunnerPath,
        [string]$BundlePath,
        [string]$CapturePath,
        [string]$EvidencePath
    )

    foreach ($value in @($RunnerPath, $BundlePath, $CapturePath, $EvidencePath)) {
        if ($value.Contains('"')) {
            throw 'Scheduled-task paths must not contain double quotes.'
        }
    }
    "-NoProfile -ExecutionPolicy Bypass -File `"$RunnerPath`" " +
        "-BundleRoot `"$BundlePath`" -CaptureScriptPath `"$CapturePath`" " +
        "-OutputDirectory `"$EvidencePath`""
}

function Invoke-InteractiveProbe {
    $interactiveOutput = Join-Path $OutputDirectory 'interactive'
    $existingTask = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    if ($null -ne $existingTask) {
        throw "Scheduled task already exists: $TaskName"
    }
    $argumentLine = Get-InteractiveArgumentLine $InteractiveRunnerPath $BundleRoot `
        $CaptureScriptPath $interactiveOutput
    $action = New-ScheduledTaskAction -Execute 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' `
        -Argument $argumentLine
    $principal = New-ScheduledTaskPrincipal -UserId $InteractiveUserId -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -ExecutionTimeLimit (New-TimeSpan -Minutes 10)
    $task = New-ScheduledTask -Action $action -Principal $principal -Settings $settings
    $startedAt = Get-Date
    $timedOut = $false
    $info = $null
    try {
        Register-ScheduledTask -TaskName $TaskName -InputObject $task -Force | Out-Null
        Start-ScheduledTask -TaskName $TaskName
        $deadline = [DateTime]::UtcNow.AddSeconds($InteractiveTimeoutSeconds)
        do {
            Start-Sleep -Milliseconds 250
            $registered = Get-ScheduledTask -TaskName $TaskName
            $info = Get-ScheduledTaskInfo -TaskName $TaskName
            if ($registered.State -ne 'Running' -and $info.LastRunTime.Year -gt 2000) {
                break
            }
        } while ([DateTime]::UtcNow -lt $deadline)
        if ($registered.State -eq 'Running' -or $info.LastRunTime.Year -le 2000) {
            $timedOut = $true
            Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        }
    } finally {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    }
    $childResultPath = Join-Path $interactiveOutput 'win32.result.json'
    $childResult = if (Test-Path -LiteralPath $childResultPath -PathType Leaf) {
        Get-Content -LiteralPath $childResultPath -Raw | ConvertFrom-Json
    } else { $null }
    $taskExitCode = if ($null -eq $info) { $null } else { [int64]$info.LastTaskResult }
    [pscustomobject]@{
        Success = -not $timedOut -and $taskExitCode -eq 0 -and $null -ne $childResult -and $childResult.Success
        StartedAt = $startedAt.ToString('o')
        CompletedAt = (Get-Date).ToString('o')
        TaskName = $TaskName
        UserId = $InteractiveUserId
        TimedOut = $timedOut
        TaskExitCode = $taskExitCode
        ChildResultPath = $childResultPath
        ChildResult = $childResult
    }
}

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'The Turnip runtime suite must run elevated.'
}
if (-not (Test-Path -LiteralPath $BundleRoot -PathType Container)) {
    throw "Turnip bundle does not exist: $BundleRoot"
}
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "Evidence directory already exists: $OutputDirectory"
}
$scriptInputs = if ($SkipInteractive) {
    @()
} else {
    @(
        @{ Path = $InteractiveRunnerPath; Hash = $expectedInteractiveRunnerHash },
        @{ Path = $CaptureScriptPath; Hash = $expectedCaptureScriptHash }
    )
}
foreach ($scriptInput in $scriptInputs) {
    if (-not (Test-Path -LiteralPath $scriptInput.Path -PathType Leaf)) {
        throw "Runtime helper does not exist: $($scriptInput.Path)"
    }
    $actualHash = (Get-FileHash -LiteralPath $scriptInput.Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $scriptInput.Hash) {
        throw "Runtime helper hash mismatch for '$($scriptInput.Path)': expected $($scriptInput.Hash), got $actualHash"
    }
}

$bundle = Assert-ExactBundle
$initialState = Get-GpuState
$initialCheck = Test-GpuState $initialState 'initial'
if (-not $initialCheck.Passed) {
    throw "Unexpected initial GPU state: $($initialCheck.Errors -join ' ')"
}
$initialBoot = [datetime](Get-CimInstance Win32_OperatingSystem).LastBootUpTime
$initialDumps = Get-DumpSnapshot
$initialDiagnostics = Get-NativeContextDiagnostics $initialState.DriverKeyPath
Remove-DestroyDiagnostics $initialState.DriverKeyPath
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null

$suiteStarted = Get-Date
$suiteErrors = [System.Collections.Generic.List[string]]::new()
$probeResults = [System.Collections.Generic.List[object]]::new()
$probeSpecs = Get-DirectProbeSpecs
$interactiveResult = $null
try {
    foreach ($spec in $probeSpecs) {
        $probeResult = Invoke-DirectProbe $spec.Name $spec.Executable $spec.Arguments $spec.Pattern
        $probeResults.Add($probeResult)
        if (-not $probeResult.Success) {
            $suiteErrors.Add("Direct Turnip probe '$($spec.Name)' failed.")
            break
        }
    }
    if ($suiteErrors.Count -eq 0 -and -not $SkipInteractive) {
        $interactiveResult = Invoke-InteractiveProbe
        if (-not $interactiveResult.Success) {
            $suiteErrors.Add('Interactive Win32 Turnip probe or visible-pixel capture failed.')
        }
    }
} catch {
    $suiteErrors.Add($_.Exception.Message)
}

$finalState = $null
$finalCheck = $null
$finalBoot = $null
$finalDumps = [ordered]@{}
$newOrChangedDumps = @()
$faultEvents = $null
$finalDiagnostics = [ordered]@{}
$destroyCheck = $null
try {
    $finalState = Get-GpuState
    $finalCheck = Test-GpuState $finalState 'final' $initialState.InstanceId
    foreach ($message in $finalCheck.Errors) {
        $suiteErrors.Add($message)
    }
    $finalBoot = [datetime](Get-CimInstance Win32_OperatingSystem).LastBootUpTime
    if ($finalBoot -ne $initialBoot) {
        $suiteErrors.Add('Windows rebooted during the Turnip runtime suite.')
    }
    $finalDumps = Get-DumpSnapshot
    $newOrChangedDumps = @(Get-NewOrChangedDumps $initialDumps $finalDumps)
    if ($newOrChangedDumps.Count -ne 0) {
        $suiteErrors.Add("The Turnip suite produced $($newOrChangedDumps.Count) new or changed dump file(s).")
    }
    $faultEvents = Get-FaultEvents -Since $suiteStarted -Until (Get-Date)
    if ($faultEvents.Count -ne 0) {
        $suiteErrors.Add("The Turnip suite produced $($faultEvents.Count) reset, bugcheck, WHEA, or application fault event(s).")
    }
    $finalDiagnostics = Get-NativeContextDiagnostics $finalState.DriverKeyPath
    $destroyCheck = Test-DestroyDiagnostics $finalDiagnostics
    foreach ($message in $destroyCheck.Errors) {
        $suiteErrors.Add($message)
    }
} catch {
    $suiteErrors.Add("Postflight collection failed: $($_.Exception.Message)")
}

$summaryPath = Join-Path $OutputDirectory 'turnip-summary.json'
$summary = [ordered]@{
    Success = $suiteErrors.Count -eq 0
    StartedAt = $suiteStarted.ToString('o')
    CompletedAt = (Get-Date).ToString('o')
    BundleRoot = $BundleRoot
    Bundle = $bundle
    InteractiveSkipped = [bool]$SkipInteractive
    InteractiveRunnerPath = if ($SkipInteractive) { $null } else { $InteractiveRunnerPath }
    InteractiveRunnerSha256 = if ($SkipInteractive) { $null } else { $expectedInteractiveRunnerHash }
    CaptureScriptPath = if ($SkipInteractive) { $null } else { $CaptureScriptPath }
    CaptureScriptSha256 = if ($SkipInteractive) { $null } else { $expectedCaptureScriptHash }
    InitialGpuState = $initialState
    InitialBootUpTime = $initialBoot.ToString('o')
    InitialDumps = $initialDumps
    InitialNativeContextDiagnostics = $initialDiagnostics
    DirectProbeResults = @($probeResults)
    InteractiveResult = $interactiveResult
    FinalGpuState = $finalState
    FinalGpuCheck = $finalCheck
    FinalBootUpTime = if ($null -eq $finalBoot) { $null } else { $finalBoot.ToString('o') }
    FinalDumps = $finalDumps
    NewOrChangedDumps = $newOrChangedDumps
    FaultEvents = $faultEvents
    FinalNativeContextDiagnostics = $finalDiagnostics
    DestroyDiagnostics = $destroyCheck
    Errors = @($suiteErrors)
}
$json = $summary | ConvertTo-Json -Depth 14
[IO.File]::WriteAllText($summaryPath, $json)
$json
if (-not $summary.Success) {
    exit 1
}
exit 0
