[CmdletBinding()]
param(
    [ValidateRange(1, 1440)]
    [int]$LookbackMinutes = 30
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$devices = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
})
if ($devices.Count -ne 1) {
    throw "Expected one present virtio-gpu device, found $($devices.Count)."
}

$device = $devices[0]
$driverProperty = Get-PnpDeviceProperty `
    -InstanceId $device.InstanceId `
    -KeyName 'DEVPKEY_Device_Driver'
$driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($driverProperty.Data)"
$driverKey = Get-Item -LiteralPath $driverKeyPath
$diagnosticNames = @(
    'NativeContextCreateStage',
    'NativeContextCreateStatus',
    'NativeContextCreateDetail',
    'NativeContextCreateFailureStage',
    'NativeContextCreateFailureStatus',
    'NativeContextCreateFailureDetail',
    'NativeContextCreateResponseType',
    'NativeContextCreateResponseSubmitted',
    'NativeContextCreateResponseCompleted',
    'NativeContextCreateResponseValidation',
    'NativeContextCreateResponseContextId',
    'NativeContextCreateResponseFlags',
    'NativeContextCreateResponseRingIndex'
)
$diagnostics = [ordered]@{}
foreach ($name in $diagnosticNames) {
    $diagnostics[$name] = $driverKey.GetValue($name, $null)
}

$since = (Get-Date).AddMinutes(-$LookbackMinutes)
$dwmCrashes = @(
    Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $since } `
        -ErrorAction SilentlyContinue |
        Where-Object { $_.Message -match '(?i)dwm\.exe|dwmcore\.dll' } |
        Sort-Object TimeCreated -Descending
)

[pscustomobject]@{
    CollectedAt = (Get-Date).ToString('o')
    DeviceStatus = [string]$device.Status
    ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) {
        $device.ProblemCode
    } else {
        $null
    }
    DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
    Diagnostics = $diagnostics
    DwmCrashCount = $dwmCrashes.Count
    LatestDwmCrash = if ($dwmCrashes.Count -gt 0) {
        [pscustomobject]@{
            TimeCreated = $dwmCrashes[0].TimeCreated.ToString('o')
            Message = $dwmCrashes[0].Message
        }
    } else {
        $null
    }
} | ConvertTo-Json -Depth 6
