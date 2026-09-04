[CmdletBinding()]
param(
    [string]$OutputPath = 'C:\DroidVM\viogpu-58044\runtime-logs\post-reboot-state.json'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$device = @(
    Get-PnpDevice -PresentOnly |
        Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' }
)
if ($device.Count -ne 1) {
    throw "Expected one present virtio-gpu device, found $($device.Count)."
}

$instanceId = $device[0].InstanceId
$problemCode = Get-PnpDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue
$driverVersion = Get-PnpDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_DriverVersion' -ErrorAction SilentlyContinue
$driverInf = Get-PnpDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue
$driverService = Get-PnpDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue
$signedDriver = Get-CimInstance Win32_PnPSignedDriver |
    Where-Object { $_.DeviceID -eq $instanceId } |
    Select-Object DeviceName, DriverVersion, InfName, DriverProviderName, IsSigned
$pendingInstallers = @(Get-Process -Name pnputil -ErrorAction SilentlyContinue | Select-Object Id, StartTime, CPU)

$result = [pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    LastBootUpTime = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
    Device = [pscustomobject]@{
        Status = $device[0].Status
        Class = $device[0].Class
        FriendlyName = $device[0].FriendlyName
        InstanceId = $instanceId
        ProblemCode = if ($null -ne $problemCode) { $problemCode.Data } else { $null }
        DriverVersion = if ($null -ne $driverVersion) { $driverVersion.Data } else { $null }
        DriverInfPath = if ($null -ne $driverInf) { $driverInf.Data } else { $null }
        Service = if ($null -ne $driverService) { $driverService.Data } else { $null }
    }
    SignedDriver = $signedDriver
    PendingInstallers = $pendingInstallers
}

$parent = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Path $parent -Force | Out-Null
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
$result | ConvertTo-Json -Depth 6
