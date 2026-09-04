[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-OptionalPnpData {
    param(
        [Parameter(Mandatory)][string]$InstanceId,
        [Parameter(Mandatory)][string]$KeyName
    )

    $property = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName $KeyName -ErrorAction SilentlyContinue
    if ($null -eq $property) { return $null }
    $dataProperty = $property.PSObject.Properties['Data']
    if ($null -eq $dataProperty) { return $null }
    return $dataProperty.Value
}

$devices = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
})
if ($devices.Count -ne 1) { throw "Expected one present virtio-gpu device, found $($devices.Count)." }
$device = $devices[0]
$driverKeyName = Get-OptionalPnpData -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
if ([string]::IsNullOrWhiteSpace([string]$driverKeyName)) { throw 'The virtio-gpu driver key is unavailable.' }
$driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$driverKeyName"
$driverKey = Get-Item -LiteralPath $driverKeyPath
$serviceKey = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
$serviceParametersPath = 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm\Parameters'
$serviceParameters = if (Test-Path -LiteralPath $serviceParametersPath) {
    Get-Item -LiteralPath $serviceParametersPath
} else {
    $null
}
$imagePath = [Environment]::ExpandEnvironmentVariables([string]$serviceKey.GetValue('ImagePath', '')).Trim('"')
if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
    $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
}
$image = Get-Item -LiteralPath $imagePath
$umd = Get-Item -LiteralPath (Join-Path $image.DirectoryName 'viogpud3d.dll')
$imageSignature = Get-AuthenticodeSignature -LiteralPath $image.FullName
$umdSignature = Get-AuthenticodeSignature -LiteralPath $umd.FullName
$service = Get-CimInstance Win32_SystemDriver -Filter "Name='VioGpuWddm'"
$signedDriver = @(Get-CimInstance Win32_PnPSignedDriver | Where-Object {
    $_.DeviceID -like 'PCI\VEN_1AF4&DEV_1050*'
})
$rollbackRoot = 'C:\Users\USER\viogpu-rollback-pre-58193-b70a09c0'
$rollbackFiles = if (Test-Path -LiteralPath $rollbackRoot -PathType Container) {
    @(Get-ChildItem -LiteralPath $rollbackRoot -File | Sort-Object Name | ForEach-Object {
        [ordered]@{
            Name = $_.Name
            Length = $_.Length
            Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
} else {
    @()
}

[ordered]@{
    Device = [ordered]@{
        Status = [string]$device.Status
        Problem = if ($null -ne $device.PSObject.Properties['Problem']) { $device.Problem } else { $null }
        ProblemCode = Get-OptionalPnpData -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode'
        ProblemStatus = Get-OptionalPnpData -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemStatus'
        InstanceId = [string]$device.InstanceId
    }
    Driver = [ordered]@{
        Version = [string]$driverKey.GetValue('DriverVersion', '')
        InfPath = [string]$driverKey.GetValue('InfPath', '')
        RenderOnly = $driverKey.GetValue('RenderOnly', $null)
        ServiceRenderOnly = $serviceKey.GetValue('RenderOnly', $null)
        ServiceParametersRenderOnly = if ($null -eq $serviceParameters) { $null } else { $serviceParameters.GetValue('RenderOnly', $null) }
        ImagePath = $image.FullName
        ImageSha256 = (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        UmdPath = $umd.FullName
        UmdSha256 = (Get-FileHash -LiteralPath $umd.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        ImageSignatureStatus = [string]$imageSignature.Status
        ImageSignerSubject = if ($null -eq $imageSignature.SignerCertificate) { $null } else { $imageSignature.SignerCertificate.Subject }
        UmdSignatureStatus = [string]$umdSignature.Status
        UmdSignerSubject = if ($null -eq $umdSignature.SignerCertificate) { $null } else { $umdSignature.SignerCertificate.Subject }
        ServiceState = [string]$service.State
        SignedDriverCount = $signedDriver.Count
        SignedDriverVersions = @($signedDriver | ForEach-Object { [string]$_.DriverVersion })
        SignedDriverInfNames = @($signedDriver | ForEach-Object { [string]$_.InfName })
    }
    Rollback = [ordered]@{
        Root = $rollbackRoot
        ManifestPresent = Test-Path -LiteralPath (Join-Path $rollbackRoot 'rollback-manifest.json') -PathType Leaf
        Files = $rollbackFiles
    }
} | ConvertTo-Json -Depth 8
