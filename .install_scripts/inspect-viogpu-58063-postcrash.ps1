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
$service = Get-CimInstance -ClassName Win32_SystemDriver -Filter "Name='VioGpuWddm'"
$imagePath = [Environment]::ExpandEnvironmentVariables([string]$service.PathName).Trim('"')
if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
    $imagePath = Join-Path $env:SystemRoot $imagePath.Substring('\SystemRoot\'.Length)
}
elseif ($imagePath.StartsWith('System32\', [StringComparison]::OrdinalIgnoreCase)) {
    $imagePath = Join-Path $env:SystemRoot $imagePath
}
$signature = Get-AuthenticodeSignature -LiteralPath $imagePath

$diagnostics = @(
    foreach ($name in $driverKey.GetValueNames() | Where-Object { $_ -like 'NativeContextCreate*' } | Sort-Object) {
        [pscustomobject]@{
            Name = $name
            Kind = [string]$driverKey.GetValueKind($name)
            Value = $driverKey.GetValue(
                $name,
                $null,
                [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames
            )
        }
    }
)

$dumpPaths = @(
    'C:\Windows\MEMORY.DMP'
    Get-ChildItem -LiteralPath 'C:\Windows\Minidump' -File -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty FullName
)
$dumps = @(
    foreach ($path in $dumpPaths) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $file = Get-Item -LiteralPath $path
            [pscustomobject]@{
                Path = $file.FullName
                Length = $file.Length
                LastWriteTimeUtc = $file.LastWriteTimeUtc.ToString('o')
            }
        }
    }
)

$captureFiles = @(
    Get-ChildItem -LiteralPath 'C:\Windows\Temp\viogpu-kmt-58063' -File -ErrorAction SilentlyContinue |
        Sort-Object Name |
        ForEach-Object {
            [pscustomobject]@{
                Path = $_.FullName
                Length = $_.Length
                LastWriteTimeUtc = $_.LastWriteTimeUtc.ToString('o')
            }
        }
)

$events = @(
    Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = (Get-Date).AddMinutes(-20) } -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -in @(41, 1001, 6008) } |
        Select-Object -First 20 TimeCreated, Id, ProviderName, Message
)

[pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    DeviceStatus = [string]$device[0].Status
    DeviceInstanceId = $device[0].InstanceId
    DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
    DriverInf = [string]$driverKey.GetValue('InfPath', '')
    ServiceState = [string]$service.State
    ImagePath = $imagePath
    ImageSha256 = (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()
    SignatureStatus = [string]$signature.Status
    Diagnostics = $diagnostics
    Dumps = $dumps
    CaptureFiles = $captureFiles
    SystemEvents = $events
} | ConvertTo-Json -Depth 8
