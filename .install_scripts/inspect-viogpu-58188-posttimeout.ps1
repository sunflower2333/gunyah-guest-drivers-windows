[CmdletBinding()]
param(
    [string]$StagingRoot = 'C:\Users\USER\viogpu-58188-d7fe1b39',
    [string]$RollbackRoot = 'C:\Users\USER\viogpu-rollback-pre-58188-d7fe1b39'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-OptionalRegistryValue {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Name
    )

    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $item = Get-Item -LiteralPath $Path
    if ($item.GetValueNames() -notcontains $Name) { return $null }
    return $item.GetValue($Name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
}

function Get-FileIdentity {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $file = Get-Item -LiteralPath $Path
    [ordered]@{
        Path = $file.FullName
        Length = $file.Length
        Sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

$devices = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
})

$deviceState = @($devices | ForEach-Object {
    [ordered]@{
        Status = [string]$_.Status
        ProblemCode = if ($null -ne $_.PSObject.Properties['ProblemCode']) { $_.ProblemCode } else { $null }
        FriendlyName = [string]$_.FriendlyName
        InstanceId = [string]$_.InstanceId
    }
})

$binding = $null
$pnpDrivers = @()
if ($devices.Count -eq 1) {
    $device = $devices[0]
    $property = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
    $driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($property.Data)"
    $driverKey = Get-Item -LiteralPath $driverKeyPath
    $service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
    $imagePath = [Environment]::ExpandEnvironmentVariables([string]$service.GetValue('ImagePath', '')).Trim('"')
    if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
        $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
    }
    $image = Get-FileIdentity -Path $imagePath
    $umdPath = if ($null -eq $image) { '' } else { Join-Path (Split-Path -Parent $image.Path) 'viogpud3d.dll' }
    $signature = if ($null -eq $image) { $null } else { Get-AuthenticodeSignature -LiteralPath $image.Path }
    $binding = [ordered]@{
        DriverKeyPath = $driverKeyPath
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        InfPath = [string]$driverKey.GetValue('InfPath', '')
        ServiceImagePath = $imagePath
        Image = $image
        Umd = if ([string]::IsNullOrEmpty($umdPath)) { $null } else { Get-FileIdentity -Path $umdPath }
        SignatureStatus = if ($null -eq $signature) { $null } else { [string]$signature.Status }
        SignerSubject = if ($null -eq $signature -or $null -eq $signature.SignerCertificate) { $null } else { $signature.SignerCertificate.Subject }
    }
    $pnpDrivers = @(& pnputil.exe /enum-devices /instanceid $device.InstanceId /drivers 2>&1)
    $pnpExitCode = $LASTEXITCODE
} else {
    $pnpExitCode = $null
}

$rollbackFiles = @()
if (Test-Path -LiteralPath $RollbackRoot -PathType Container) {
    $rollbackFiles = @(Get-ChildItem -LiteralPath $RollbackRoot -Recurse -File | ForEach-Object {
        [ordered]@{
            Name = $_.Name
            RelativePath = $_.FullName.Substring($RollbackRoot.Length).TrimStart('\')
            Length = $_.Length
            Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
}

$stagedFiles = @()
if (Test-Path -LiteralPath $StagingRoot -PathType Container) {
    $stagedFiles = @(Get-ChildItem -LiteralPath $StagingRoot -Recurse -File | ForEach-Object {
        [ordered]@{
            Name = $_.Name
            RelativePath = $_.FullName.Substring($StagingRoot.Length).TrimStart('\')
            Length = $_.Length
            Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
}

$pendingFileRenamePath = 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Session Manager'
$pending = [ordered]@{
    CbsRebootPending = Test-Path -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending'
    WindowsUpdateRebootRequired = Test-Path -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired'
    PnpRebootRequired = Test-Path -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\PnP\PnpRebootRequired'
    PendingFileRenameOperations = Get-OptionalRegistryValue -Path $pendingFileRenamePath -Name 'PendingFileRenameOperations'
}

$serviceState = Get-CimInstance Win32_SystemDriver -Filter "Name='VioGpuWddm'" | Select-Object Name,State,Status,StartMode,PathName
$signedDriver = @(Get-CimInstance Win32_PnPSignedDriver | Where-Object {
    $_.DeviceID -like 'PCI\VEN_1AF4&DEV_1050*'
} | Select-Object DeviceID,DriverVersion,InfName,DriverDate,Manufacturer)

[ordered]@{
    CollectedAt = (Get-Date).ToString('o')
    DeviceCount = $devices.Count
    Devices = $deviceState
    Binding = $binding
    Service = $serviceState
    SignedDriver = $signedDriver
    PnpDriversExitCode = $pnpExitCode
    PnpDriversOutput = $pnpDrivers
    PendingReboot = $pending
    RollbackRootExists = Test-Path -LiteralPath $RollbackRoot -PathType Container
    RollbackFiles = $rollbackFiles
    StagedFiles = $stagedFiles
} | ConvertTo-Json -Depth 10
