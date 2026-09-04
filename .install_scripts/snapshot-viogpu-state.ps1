[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$devices = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
})
if ($devices.Count -ne 1) { throw "Expected one present virtio-gpu device, found $($devices.Count)." }
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
$storeDir = $image.DirectoryName
$signature = Get-AuthenticodeSignature -LiteralPath $image.FullName

$storeFiles = @(Get-ChildItem -LiteralPath $storeDir -File | ForEach-Object {
    [pscustomobject]@{
        Name = $_.Name
        Length = $_.Length
        Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})

[ordered]@{
    CollectedAt = (Get-Date).ToString('o')
    ComputerName = $env:COMPUTERNAME
    Status = [string]$device.Status
    InstanceId = [string]$device.InstanceId
    ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) { [string]$device.ProblemCode } else { $null }
    DriverKeyPath = $driverKeyPath
    DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
    InfPath = [string]$driverKey.GetValue('InfPath', '')
    ImagePath = $image.FullName
    DriverStoreDirectory = $storeDir
    SignatureStatus = [string]$signature.Status
    SignerSubject = if ($null -eq $signature.SignerCertificate) { $null } else { [string]$signature.SignerCertificate.Subject }
    ServiceState = [string](Get-Service VioGpuWddm).Status
    DriverStoreFiles = $storeFiles
} | ConvertTo-Json -Depth 6
