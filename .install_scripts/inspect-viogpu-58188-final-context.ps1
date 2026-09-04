[CmdletBinding()]
param()

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
$service = Get-CimInstance Win32_SystemDriver -Filter "Name='VioGpuWddm'"
$nativeContextValues = [ordered]@{}

foreach ($name in @($driverKey.GetValueNames() | Sort-Object)) {
    if ($name -match '^NativeContext') {
        $nativeContextValues[$name] = $driverKey.GetValue($name, $null)
    }
}

[pscustomobject]@{
    CollectedAt = (Get-Date).ToString('o')
    BootTime = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
    Device = [ordered]@{
        InstanceId = [string]$device.InstanceId
        Status = [string]$device.Status
        ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) {
            $device.ProblemCode
        } else {
            $null
        }
        DriverKey = [string]$driverProperty.Data
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
    }
    Service = [ordered]@{
        State = [string]$service.State
        Status = [string]$service.Status
        PathName = [string]$service.PathName
    }
    NativeContextValues = $nativeContextValues
} | ConvertTo-Json -Depth 8
