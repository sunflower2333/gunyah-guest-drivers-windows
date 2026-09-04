[CmdletBinding()]
param(
    [string]$OutputDirectory = 'C:\DroidVM\viogpu-58029\runtime-logs',
    [int]$RestartTimeoutSeconds = 45
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$devices = @(
    Get-PnpDevice -PresentOnly |
        Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' }
)
if ($devices.Count -ne 1) {
    throw "Expected one present virtio-gpu device, found $($devices.Count)."
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Get-ChildItem -LiteralPath $OutputDirectory -File -ErrorAction Stop | Remove-Item -Force

$stamp = Get-Date -Format 'yyyyMMdd-HHmmssfff'
$traceName = "DroidVM-VioGpu-$stamp"
$etlPath = Join-Path $OutputDirectory "viogpu-bounded-restart-$stamp.etl"
$stdoutPath = Join-Path $OutputDirectory "pnputil-$stamp.stdout.txt"
$stderrPath = Join-Path $OutputDirectory "pnputil-$stamp.stderr.txt"
$resultPath = Join-Path $OutputDirectory "viogpu-bounded-restart-$stamp.json"
$traceStarted = $false
$restartProcess = $null
$restartTimedOut = $false

try {
    & logman.exe create trace $traceName -ow -o $etlPath `
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

    $restartProcess = Start-Process -FilePath "$env:SystemRoot\System32\pnputil.exe" `
        -ArgumentList @('/restart-device', $devices[0].InstanceId) `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru
    if (-not $restartProcess.WaitForExit($RestartTimeoutSeconds * 1000)) {
        $restartTimedOut = $true
        Stop-Process -Id $restartProcess.Id -Force -ErrorAction SilentlyContinue
        $restartProcess.WaitForExit(5000) | Out-Null
    }
    Start-Sleep -Seconds 3
}
finally {
    if ($traceStarted) {
        & logman.exe stop $traceName -ets | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Error "logman stop failed with exit code $LASTEXITCODE."
        }
    }
}

$deviceAfter = Get-PnpDevice -InstanceId $devices[0].InstanceId
$problemProperty = Get-PnpDeviceProperty -InstanceId $devices[0].InstanceId `
    -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue
$problemCode = if ($null -ne $problemProperty -and $null -ne $problemProperty.PSObject.Properties['Data']) {
    $problemProperty.Data
}
else {
    $null
}

$result = [pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    TraceName = $traceName
    EtlPath = $etlPath
    EtlLength = (Get-Item -LiteralPath $etlPath).Length
    PnpUtilProcessId = if ($null -eq $restartProcess) { $null } else { $restartProcess.Id }
    PnpUtilTimedOut = $restartTimedOut
    PnpUtilExitCode = if ($null -eq $restartProcess -or $restartTimedOut) { $null } else { $restartProcess.ExitCode }
    PnpUtilStdout = @(Get-Content -LiteralPath $stdoutPath -ErrorAction SilentlyContinue)
    PnpUtilStderr = @(Get-Content -LiteralPath $stderrPath -ErrorAction SilentlyContinue)
    DeviceStatusAfter = $deviceAfter.Status
    ProblemCodeAfter = $problemCode
}
$result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $resultPath -Encoding UTF8
$result | ConvertTo-Json -Depth 4
