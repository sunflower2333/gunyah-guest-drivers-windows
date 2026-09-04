[CmdletBinding()]
param(
    [string]$BundleRoot = 'C:\DroidVM\TurnipRuns\run-33322445949\turnip-wddm-arm64-icd-bundle',
    [string]$CaptureScriptPath = 'C:\DroidVM\TurnipRuns\viogpu-58186-c913fd87\capture-turnip-wddm-win32-window.ps1',
    [string]$OutputDirectory = 'C:\DroidVM\TurnipRuns\evidence\viogpu-58188-win32-interactive',
    [ValidateRange(2, 50)]
    [int]$Iterations = 10,
    [ValidateRange(5, 120)]
    [int]$PerIterationTimeoutSeconds = 30,
    [ValidateRange(10, 180)]
    [int]$CaptureWaitSeconds = 90
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedDriverVersion = '100.6.101.58188'
$expectedDriverHash = 'b699bb08b055034fe72b1e6676993fb0d0152f663330fa9c34db0820231d40ca'
$expectedUmdHash = 'd89f3d930dfe4d0eb3480f3b7495243e7c6805eb3d94d11fc497e7c180345474'
$expectedProbeHash = '848b4ae08c97d9d3d8db1408f04b865e192e8df014fd92e5f557b631184271b4'
$expectedCaptureHash = '560da1a81144ffc7d6432b01378d21baa7cdf962d69906ab2d96744925c5f77e'

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
    param([object]$State, [string]$ExpectedInstanceId = '')

    $stateErrors = [System.Collections.Generic.List[string]]::new()
    if ($State.Status -ne 'OK') {
        $stateErrors.Add("Device status is '$($State.Status)', expected 'OK'.")
    }
    if ($null -ne $State.ProblemCode -and [int]$State.ProblemCode -ne 0) {
        $stateErrors.Add("Problem code is '$($State.ProblemCode)', expected 0.")
    }
    if ($State.DriverVersion -ne $expectedDriverVersion) {
        $stateErrors.Add("Driver version is '$($State.DriverVersion)', expected '$expectedDriverVersion'.")
    }
    if ($State.DriverImageSha256 -ne $expectedDriverHash) {
        $stateErrors.Add("Driver hash is '$($State.DriverImageSha256)', expected '$expectedDriverHash'.")
    }
    if ($State.UmdSha256 -ne $expectedUmdHash) {
        $stateErrors.Add("UMD hash is '$($State.UmdSha256)', expected '$expectedUmdHash'.")
    }
    if ($State.DriverServiceStatus -ne 'Running') {
        $stateErrors.Add("VioGpuWddm service is '$($State.DriverServiceStatus)', expected 'Running'.")
    }
    if (-not [string]::IsNullOrEmpty($ExpectedInstanceId) -and $State.InstanceId -ne $ExpectedInstanceId) {
        $stateErrors.Add("Device instance changed from '$ExpectedInstanceId' to '$($State.InstanceId)'.")
    }
    [pscustomobject]@{
        Passed = $stateErrors.Count -eq 0
        Errors = @($stateErrors)
    }
}

if (Test-Path -LiteralPath $OutputDirectory) {
    throw "Evidence directory already exists: $OutputDirectory"
}
$probePath = Join-Path $BundleRoot 'tu_wddm_win32_probe_arm64.exe'
foreach ($inputPath in @($probePath, $CaptureScriptPath)) {
    if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) {
        throw "Interactive input does not exist: $inputPath"
    }
}
$probeHash = (Get-FileHash -LiteralPath $probePath -Algorithm SHA256).Hash.ToLowerInvariant()
$captureHash = (Get-FileHash -LiteralPath $CaptureScriptPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($probeHash -ne $expectedProbeHash) {
    throw "Win32 probe SHA-256 mismatch: expected $expectedProbeHash, got $probeHash"
}
if ($captureHash -ne $expectedCaptureHash) {
    throw "Capture script SHA-256 mismatch: expected $expectedCaptureHash, got $captureHash"
}

$before = Get-GpuState
$beforeCheck = Test-GpuState $before
if (-not $beforeCheck.Passed) {
    throw "Unexpected pre-probe GPU state: $($beforeCheck.Errors -join ' ')"
}
$currentSessionId = (Get-Process -Id $PID).SessionId
if ($currentSessionId -eq 0) {
    throw 'The interactive Win32 runner must execute outside session 0.'
}

New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$screenshotPath = Join-Path $OutputDirectory 'win32-visible.png'
$captureMetadataPath = Join-Path $OutputDirectory 'win32-visible.json'
$captureStdoutPath = Join-Path $OutputDirectory 'capture.stdout.txt'
$captureStderrPath = Join-Path $OutputDirectory 'capture.stderr.txt'
$combinedStdoutPath = Join-Path $OutputDirectory 'win32.stdout.txt'
$combinedStderrPath = Join-Path $OutputDirectory 'win32.stderr.txt'
$resultPath = Join-Path $OutputDirectory 'win32.result.json'
$startedAt = Get-Date
$errors = [System.Collections.Generic.List[string]]::new()
$iterationResults = [System.Collections.Generic.List[object]]::new()
$combinedStdout = [System.Text.StringBuilder]::new()
$combinedStderr = [System.Text.StringBuilder]::new()

$captureArguments = @(
    '-NoProfile',
    '-ExecutionPolicy', 'Bypass',
    '-File', ('"{0}"' -f $CaptureScriptPath),
    '-OutputPath', ('"{0}"' -f $screenshotPath),
    '-MetadataPath', ('"{0}"' -f $captureMetadataPath),
    '-WaitSeconds', [string]$CaptureWaitSeconds
)
$capture = Start-Process -FilePath 'powershell.exe' -ArgumentList $captureArguments -PassThru `
    -RedirectStandardOutput $captureStdoutPath -RedirectStandardError $captureStderrPath

try {
    for ($iteration = 1; $iteration -le $Iterations; $iteration++) {
        $stdoutPath = Join-Path $OutputDirectory ('iteration-{0:D2}.stdout.txt' -f $iteration)
        $stderrPath = Join-Path $OutputDirectory ('iteration-{0:D2}.stderr.txt' -f $iteration)
        $process = Start-Process -FilePath $probePath -WorkingDirectory $BundleRoot -PassThru `
            -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
        $timedOut = -not $process.WaitForExit($PerIterationTimeoutSeconds * 1000)
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
        [void]$combinedStdout.Append($stdout)
        [void]$combinedStderr.Append($stderr)
        $markerPassed = $stdout -match '(?m)^tu WDDM Win32 probe passed: clear/submit/present [0-9]+x[0-9]+ -> [0-9]+x[0-9]+ \([0-9]+/[0-9]+ images\)\s*$'
        $iterationPassed = -not $timedOut -and $exitCode -eq 0 -and $markerPassed -and [string]::IsNullOrWhiteSpace($stderr)
        $iterationResults.Add([pscustomobject]@{
            Iteration = $iteration
            ProcessId = $process.Id
            TimedOut = $timedOut
            ExitCode = $exitCode
            MarkerPassed = $markerPassed
            StderrEmpty = [string]::IsNullOrWhiteSpace($stderr)
            Success = $iterationPassed
            StdoutPath = $stdoutPath
            StderrPath = $stderrPath
        })
        if (-not $iterationPassed) {
            $errors.Add("Interactive iteration $iteration failed: timeout=$timedOut exit=$exitCode marker=$markerPassed.")
            break
        }
    }
} finally {
    if (-not $capture.HasExited) {
        [void]$capture.WaitForExit(15000)
    }
    if (-not $capture.HasExited) {
        & taskkill.exe /PID $capture.Id /T /F *> $null
        [void]$capture.WaitForExit(5000)
    }
}

[IO.File]::WriteAllText($combinedStdoutPath, $combinedStdout.ToString())
[IO.File]::WriteAllText($combinedStderrPath, $combinedStderr.ToString())
$capture.Refresh()
$captureExitCode = if ($capture.HasExited) { [int]$capture.ExitCode } else { 124 }
if ($captureExitCode -ne 0) {
    $errors.Add("Visible capture exited with code $captureExitCode.")
}
$captureMetadata = $null
if (-not (Test-Path -LiteralPath $screenshotPath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $captureMetadataPath -PathType Leaf)) {
    $errors.Add('Visible capture did not produce both PNG and metadata evidence.')
} else {
    $captureMetadata = Get-Content -LiteralPath $captureMetadataPath -Raw | ConvertFrom-Json
    if ($captureMetadata.ProbeSessionId -eq 0 -or $captureMetadata.TargetSamples -lt $captureMetadata.MinimumTargetSamples) {
        $errors.Add('Visible capture metadata does not prove a non-session-0 target-color window.')
    }
}

$after = Get-GpuState
$afterCheck = Test-GpuState $after $before.InstanceId
foreach ($message in $afterCheck.Errors) {
    $errors.Add($message)
}
$passedIterations = @($iterationResults | Where-Object { $_.Success }).Count
if ($passedIterations -ne $Iterations) {
    $errors.Add("Only $passedIterations of $Iterations interactive iterations passed.")
}

$result = [ordered]@{
    Success = $errors.Count -eq 0
    StartedAt = $startedAt.ToString('o')
    CompletedAt = (Get-Date).ToString('o')
    RunnerProcessId = $PID
    RunnerSessionId = $currentSessionId
    BundleRoot = $BundleRoot
    ProbePath = $probePath
    ProbeSha256 = $probeHash
    CaptureScriptPath = $CaptureScriptPath
    CaptureScriptSha256 = $captureHash
    IterationsRequested = $Iterations
    IterationsPassed = $passedIterations
    IterationResults = @($iterationResults)
    CaptureProcessId = $capture.Id
    CaptureExitCode = $captureExitCode
    CaptureMetadata = $captureMetadata
    ScreenshotPath = $screenshotPath
    ScreenshotSha256 = if (Test-Path -LiteralPath $screenshotPath -PathType Leaf) {
        (Get-FileHash -LiteralPath $screenshotPath -Algorithm SHA256).Hash.ToLowerInvariant()
    } else { $null }
    CombinedStdoutPath = $combinedStdoutPath
    CombinedStderrPath = $combinedStderrPath
    GpuBefore = $before
    GpuAfter = $after
    Errors = @($errors)
}
$json = $result | ConvertTo-Json -Depth 10
[IO.File]::WriteAllText($resultPath, $json)
$json
if (-not $result.Success) {
    exit 1
}
exit 0
