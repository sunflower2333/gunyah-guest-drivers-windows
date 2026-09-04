[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$device = @(
    Get-PnpDevice -PresentOnly |
        Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' }
)
if ($device.Count -ne 1) {
    throw "Expected exactly one present virtio-gpu device, found $($device.Count)."
}

$driverProperty = Get-PnpDeviceProperty -InstanceId $device[0].InstanceId -KeyName 'DEVPKEY_Device_Driver'
$driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($driverProperty.Data)"
$driverKey = Get-Item -LiteralPath $driverKeyPath

$paths = @(
    'C:\Windows\MEMORY.DMP'
    'C:\Windows\Minidump'
    'C:\Windows\Temp\viogpu-kmt-58063'
    'C:\Windows\Temp\viogpu-pre-58063-archive'
    'C:\Users\Administrator\tu_wddm_kmt_probe_arm64.exe'
    'C:\Users\Administrator\capture-viogpu-kmt-etl.ps1'
)

$files = @(
    foreach ($path in $paths) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $file = Get-Item -LiteralPath $path -Force
            [pscustomobject]@{
                Path = $file.FullName
                Length = $file.Length
                LastWriteTimeUtc = $file.LastWriteTimeUtc.ToString('o')
            }
        }
        elseif (Test-Path -LiteralPath $path -PathType Container) {
            foreach ($file in Get-ChildItem -LiteralPath $path -File -Recurse -Force -ErrorAction SilentlyContinue) {
                [pscustomobject]@{
                    Path = $file.FullName
                    Length = $file.Length
                    LastWriteTimeUtc = $file.LastWriteTimeUtc.ToString('o')
                }
            }
        }
    }
)

[pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    DeviceStatus = [string]$device[0].Status
    DeviceInstanceId = $device[0].InstanceId
    DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
    Files = $files
} | ConvertTo-Json -Depth 6
