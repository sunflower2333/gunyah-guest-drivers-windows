$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$device = @(
    Get-PnpDevice -PresentOnly |
        Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' }
)
if ($device.Count -ne 1) {
    throw "Expected one present virtio-gpu device, found $($device.Count)."
}

$properties = [ordered]@{}
foreach ($key in @(
    'DEVPKEY_Device_ProblemCode',
    'DEVPKEY_Device_ProblemStatus',
    'DEVPKEY_Device_Driver',
    'DEVPKEY_Device_DriverVersion',
    'DEVPKEY_Device_DriverInfPath'
)) {
    $property = Get-PnpDeviceProperty -InstanceId $device[0].InstanceId -KeyName $key -ErrorAction SilentlyContinue
    $properties[$key] = if ($null -ne $property -and $null -ne $property.PSObject.Properties['Data']) {
        $property.Data
    }
    else {
        $null
    }
}

$pnputilProcesses = @(
    Get-Process -Name pnputil -ErrorAction SilentlyContinue |
        Select-Object Id, ProcessName, StartTime, CPU, Responding
)
$displayDriverStore = @(& pnputil.exe /enum-drivers /class Display 2>&1)
$displayDriverStoreExitCode = $LASTEXITCODE

[pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    Device = [pscustomobject]@{
        InstanceId = $device[0].InstanceId
        Status = $device[0].Status
        Properties = [pscustomobject]$properties
    }
    PnpUtilProcesses = $pnputilProcesses
    DisplayDriverStoreExitCode = $displayDriverStoreExitCode
    DisplayDriverStore = $displayDriverStore
    InstallResultExists = Test-Path -LiteralPath 'C:\DroidVM\viogpu-58029\runtime-logs\install-result.json'
} | ConvertTo-Json -Depth 6
