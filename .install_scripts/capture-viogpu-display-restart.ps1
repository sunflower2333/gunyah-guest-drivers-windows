[CmdletBinding()]
param(
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $PSScriptRoot 'runtime-logs'
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$devices = @(
    Get-PnpDevice -PresentOnly |
        Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' }
)
if ($devices.Count -ne 1) {
    throw "Expected one present virtio-gpu device, found $($devices.Count)."
}

$instanceId = $devices[0].InstanceId
$stamp = Get-Date -Format 'yyyyMMdd-HHmmssfff'
$traceName = "DroidVM-VioGpu-$stamp"
$etlPath = Join-Path $OutputDirectory "viogpu-display-restart-$stamp.etl"
$resultPath = Join-Path $OutputDirectory "viogpu-display-restart-$stamp.json"
$traceStarted = $false
$restartOutput = @()
$restartExitCode = $null
$deviceAfter = $null
$problemCodeAfter = $null

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

    $restartOutput = @(& pnputil.exe /restart-device $instanceId 2>&1)
    $restartExitCode = $LASTEXITCODE
    if ($restartExitCode -ne 0) {
        throw "pnputil restart failed with exit code $restartExitCode."
    }

    Start-Sleep -Seconds 8
    $deviceAfter = Get-PnpDevice -InstanceId $instanceId
    $problemProperty = Get-PnpDeviceProperty -InstanceId $instanceId `
        -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue
    if ($null -ne $problemProperty -and $null -ne $problemProperty.PSObject.Properties['Data']) {
        $problemCodeAfter = $problemProperty.Data
    }
}
finally {
    if ($traceStarted) {
        & logman.exe stop $traceName -ets | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Error "logman stop failed with exit code $LASTEXITCODE."
        }
    }
}

$result = [pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    TraceName = $traceName
    EtlPath = $etlPath
    EtlLength = (Get-Item -LiteralPath $etlPath).Length
    InstanceId = $instanceId
    PnpUtilExitCode = $restartExitCode
    PnpUtilOutput = $restartOutput
    DeviceStatusAfter = $deviceAfter.Status
    ProblemCodeAfter = $problemCodeAfter
}
$result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $resultPath -Encoding UTF8
$result | ConvertTo-Json -Depth 4
