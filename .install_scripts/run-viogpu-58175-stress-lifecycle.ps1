[CmdletBinding()]
param(
    [string]$ProbePath = 'C:\Users\USER\viogpu-58175-18fa0890\kmt\tu_wddm_kmt_probe_df9b0cf7_arm64.exe',
    [string]$OutputDirectory = 'C:\Users\USER\viogpu-58175-18fa0890\stress',
    [int]$ProbeTimeoutMilliseconds = 120000
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedProbeHash = '013dbee453a5506df456ee35cd86a79c0fccd015864ab38fd2a24e85381eee87'
$expectedDriverVersion = '100.6.101.58175'
$expectedDriverHash = '4dbb8cd63c9bcd833db62e302ab69c2dc39adddf7177d3b10ae880bcc942451a'

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
    $service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
    $imagePath = [Environment]::ExpandEnvironmentVariables([string]$service.GetValue('ImagePath', '')).Trim('"')
    if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
        $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
    }
    [pscustomobject]@{
        Status = [string]$device.Status
        ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) { $device.ProblemCode } else { $null }
        InstanceId = [string]$device.InstanceId
        DriverKeyPath = $driverKeyPath
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        DriverImagePath = $imagePath
        DriverImageSha256 = (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Get-RangeDiagnostics {
    param([string]$DriverKeyPath)
    $key = Get-Item -LiteralPath $DriverKeyPath
    $values = [ordered]@{}
    foreach ($name in @($key.GetValueNames() | Sort-Object)) {
        if ($name.StartsWith('NativeContextAllocationRange', [StringComparison]::Ordinal)) {
            $values[$name] = $key.GetValue(
                $name,
                $null,
                [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames
            )
        }
    }
    $values
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

$before = Get-GpuState
if ($before.Status -ne 'OK' -or
    ($null -ne $before.ProblemCode -and $before.ProblemCode -ne 0) -or
    $before.DriverVersion -ne $expectedDriverVersion -or
    $before.DriverImageSha256 -ne $expectedDriverHash) {
    throw "Unexpected pre-probe GPU state: $($before | ConvertTo-Json -Compress)"
}
$diagnosticsBefore = Get-RangeDiagnostics $before.DriverKeyPath
foreach ($name in @($diagnosticsBefore.Keys)) {
    Remove-ItemProperty -LiteralPath $before.DriverKeyPath -Name $name -ErrorAction Stop
}

New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmssfff'
$traceName = "DroidVM-VioGpu-58175-Stress-$stamp"
$etlPath = Join-Path $OutputDirectory "stress-$stamp.etl"
$stdoutPath = Join-Path $OutputDirectory "stress-$stamp.stdout.txt"
$stderrPath = Join-Path $OutputDirectory "stress-$stamp.stderr.txt"
$resultPath = Join-Path $OutputDirectory "stress-$stamp.result.json"
$traceStarted = $false
$probeProcessId = $null
$probeExitCode = $null
$timedOut = $false
$startedAt = Get-Date

try {
    & logman.exe create trace $traceName -ow -o $etlPath -f bincirc -max 16 -bs 64 -nb 2 8 -ft 00:00:01 `
        -p Microsoft-Windows-DxgKrnl 0xFFFFFFFFFFFFFFFF 0xFF -ets | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "logman create failed with exit code $LASTEXITCODE."
    }
    $traceStarted = $true
    & logman.exe update $traceName -p '{D6B96B2C-72BF-4CA5-BB89-9FCA5C82F020}' 0x7FFFFFFF 0xFF -ets | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "logman provider update failed with exit code $LASTEXITCODE."
    }
    Start-Sleep -Seconds 2
    $probe = Start-Process -FilePath $ProbePath -WorkingDirectory (Split-Path $ProbePath) `
        -ArgumentList @('--stage=allocation', '--stress-lifecycle') -PassThru `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    $probeProcessId = $probe.Id
    $timedOut = -not $probe.WaitForExit($ProbeTimeoutMilliseconds)
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
} finally {
    if ($traceStarted) {
        & logman.exe stop $traceName -ets | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "logman stop failed with exit code $LASTEXITCODE."
        }
    }
}

$after = Get-GpuState
$diagnosticsAfter = Get-RangeDiagnostics $after.DriverKeyPath
$stdout = if (Test-Path -LiteralPath $stdoutPath -PathType Leaf) { [IO.File]::ReadAllText($stdoutPath) } else { '' }
$stderr = if (Test-Path -LiteralPath $stderrPath -PathType Leaf) { [IO.File]::ReadAllText($stderrPath) } else { '' }
$probeReportedPass = $stdout -match '(?m)^tu WDDM KMT probe passed stage=allocation\s*$'
if ($probeExitCode -eq 0 -and -not $probeReportedPass) {
    $probeExitCode = 1
}
$result = [ordered]@{
    StartedAt = $startedAt.ToString('o')
    CompletedAt = (Get-Date).ToString('o')
    ProbeStage = 'allocation'
    ProbeOptions = @('--stress-lifecycle')
    ProbePath = $ProbePath
    ProbeSha256 = $probeHash
    ProbeProcessId = $probeProcessId
    ProbeTimedOut = $timedOut
    ProbeExitCode = $probeExitCode
    ProbeReportedPass = $probeReportedPass
    EtlPath = $etlPath
    EtlLength = if (Test-Path -LiteralPath $etlPath -PathType Leaf) { (Get-Item -LiteralPath $etlPath).Length } else { 0 }
    StdoutPath = $stdoutPath
    StderrPath = $stderrPath
    Stdout = $stdout
    Stderr = $stderr
    DiagnosticsBefore = $diagnosticsBefore
    DiagnosticsAfter = $diagnosticsAfter
    GpuBefore = $before
    GpuAfter = $after
}
$json = $result | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText($resultPath, $json)
$json
exit $probeExitCode
