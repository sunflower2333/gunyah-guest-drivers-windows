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
$driverProperty = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
$driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($driverProperty.Data)"
$driverKey = Get-Item -LiteralPath $driverKeyPath
$service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
$imagePath = [Environment]::ExpandEnvironmentVariables([string]$service.GetValue('ImagePath', '')).Trim('"')
if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
    $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
}
$image = Get-Item -LiteralPath $imagePath
$umdPath = Join-Path $image.DirectoryName 'viogpud3d.dll'
$signature = Get-AuthenticodeSignature -LiteralPath $image.FullName

$diagnostics = [ordered]@{}
foreach ($name in @($driverKey.GetValueNames() | Where-Object {
    $_.StartsWith('NativeContext', [StringComparison]::Ordinal)
} | Sort-Object)) {
    $diagnostics[$name] = $driverKey.GetValue(
        $name,
        $null,
        [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames
    )
}

[ordered]@{
    CapturedAt = (Get-Date).ToString('o')
    ComputerName = $env:COMPUTERNAME
    OsBuild = [Environment]::OSVersion.Version.ToString()
    Status = [string]$device.Status
    ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) { $device.ProblemCode } else { $null }
    InstanceId = [string]$device.InstanceId
    DriverKeyPath = $driverKeyPath
    DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
    InfPath = [string]$driverKey.GetValue('InfPath', '')
    ImagePath = $image.FullName
    ImageSha256 = (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    UmdPath = $umdPath
    UmdSha256 = if (Test-Path -LiteralPath $umdPath -PathType Leaf) {
        (Get-FileHash -LiteralPath $umdPath -Algorithm SHA256).Hash.ToLowerInvariant()
    } else { $null }
    SignatureStatus = [string]$signature.Status
    SignerSubject = if ($null -eq $signature.SignerCertificate) { $null } else { $signature.SignerCertificate.Subject }
    NativeContextDiagnostics = $diagnostics
} | ConvertTo-Json -Depth 8
