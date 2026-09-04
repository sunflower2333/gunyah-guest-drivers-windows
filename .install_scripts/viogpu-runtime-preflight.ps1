[CmdletBinding()]
param(
    [string]$DevicePattern = 'PCI\VEN_1AF4&DEV_1050*'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-DevicePropertyValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstanceId,

        [Parameter(Mandatory = $true)]
        [string]$KeyName
    )

    $property = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName $KeyName -ErrorAction SilentlyContinue
    if ($null -eq $property -or $null -eq $property.PSObject.Properties['Data']) {
        return $null
    }
    return $property.Data
}

$devices = @(
    Get-PnpDevice -PresentOnly |
        Where-Object { $_.InstanceId -like $DevicePattern }
)

$deviceState = @(
    foreach ($device in $devices) {
        [pscustomobject]@{
            InstanceId = $device.InstanceId
            Status = $device.Status
            Class = $device.Class
            FriendlyName = $device.FriendlyName
            ProblemCode = Get-DevicePropertyValue $device.InstanceId 'DEVPKEY_Device_ProblemCode'
            ProblemStatus = Get-DevicePropertyValue $device.InstanceId 'DEVPKEY_Device_ProblemStatus'
            DriverKey = Get-DevicePropertyValue $device.InstanceId 'DEVPKEY_Device_Driver'
            DriverVersion = Get-DevicePropertyValue $device.InstanceId 'DEVPKEY_Device_DriverVersion'
            DriverDate = Get-DevicePropertyValue $device.InstanceId 'DEVPKEY_Device_DriverDate'
            DriverProvider = Get-DevicePropertyValue $device.InstanceId 'DEVPKEY_Device_DriverProvider'
            InfPath = Get-DevicePropertyValue $device.InstanceId 'DEVPKEY_Device_DriverInfPath'
        }
    }
)

$signedDisplayDrivers = @(
    Get-CimInstance Win32_PnPSignedDriver |
        Where-Object { $_.DeviceClass -eq 'DISPLAY' } |
        Select-Object DeviceName, DeviceID, DriverVersion, DriverDate, InfName, Manufacturer, IsSigned
)

$diagnosticFiles = @()
$diagnosticPaths = @(
    'C:\Windows\MEMORY.DMP',
    'C:\Windows\Minidump',
    'C:\Users\Administrator',
    'C:\DroidVM',
    'C:\Temp'
)
foreach ($path in $diagnosticPaths) {
    if (-not (Test-Path -LiteralPath $path)) {
        continue
    }

    $item = Get-Item -LiteralPath $path
    if (-not $item.PSIsContainer) {
        $diagnosticFiles += $item
        continue
    }

    $diagnosticFiles += Get-ChildItem -LiteralPath $path -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in @('.dmp', '.etl') }
}

$os = Get-CimInstance Win32_OperatingSystem
$displayDriverPackages = @(& pnputil.exe /enum-drivers /class Display)

[pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    ComputerName = $env:COMPUTERNAME
    LastBootUpTime = $os.LastBootUpTime
    DeviceCount = $devices.Count
    Devices = $deviceState
    SignedDisplayDrivers = $signedDisplayDrivers
    DiagnosticFiles = @(
        $diagnosticFiles |
            Sort-Object FullName -Unique |
            Select-Object FullName, Length, CreationTimeUtc, LastWriteTimeUtc
    )
    DisplayDriverPackages = $displayDriverPackages
} | ConvertTo-Json -Depth 6
