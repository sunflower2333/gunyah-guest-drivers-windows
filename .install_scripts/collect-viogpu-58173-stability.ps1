[CmdletBinding()]
param(
    [string]$OutputPath = 'C:\Users\USER\viogpu-58173-c5c742f6\post-probe-stability.json'
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
$property = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
$driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($property.Data)"
$driverKey = Get-Item -LiteralPath $driverKeyPath
$service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
$imagePath = [Environment]::ExpandEnvironmentVariables([string]$service.GetValue('ImagePath', '')).Trim('"')
if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
    $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
}
$image = Get-Item -LiteralPath $imagePath
$umdPath = Join-Path $image.DirectoryName 'viogpud3d.dll'

$nativeContext = [ordered]@{}
foreach ($name in @($driverKey.GetValueNames() | Where-Object {
    $_.StartsWith('NativeContext', [StringComparison]::Ordinal)
} | Sort-Object)) {
    $nativeContext[$name] = $driverKey.GetValue($name, $null)
}

$recentEvents = @(
    Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = (Get-Date).AddMinutes(-15) } -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -in @(41, 1001, 17, 18, 19, 14, 4101) } |
        Select-Object -First 40 TimeCreated, Id, ProviderName, LevelDisplayName, Message
)

$os = Get-CimInstance -ClassName Win32_OperatingSystem
$result = [ordered]@{
    CollectedAt = (Get-Date).ToString('o')
    LastBootUpTime = ([datetime]$os.LastBootUpTime).ToString('o')
    Device = [ordered]@{
        Status = [string]$device.Status
        ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) { $device.ProblemCode } else { $null }
        InstanceId = [string]$device.InstanceId
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        InfPath = [string]$driverKey.GetValue('InfPath', '')
        ImagePath = $image.FullName
        ImageSha256 = (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        UmdPath = $umdPath
        UmdSha256 = (Get-FileHash -LiteralPath $umdPath -Algorithm SHA256).Hash.ToLowerInvariant()
        Signature = [ordered]@{
            Status = [string](Get-AuthenticodeSignature -LiteralPath $image.FullName).Status
            Subject = if ($null -eq (Get-AuthenticodeSignature -LiteralPath $image.FullName).SignerCertificate) {
                $null
            } else {
                (Get-AuthenticodeSignature -LiteralPath $image.FullName).SignerCertificate.Subject
            }
        }
        NativeContextDiagnostics = $nativeContext
    }
    RecentSystemEvents = $recentEvents
}

$json = $result | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText($OutputPath, $json)
$json
