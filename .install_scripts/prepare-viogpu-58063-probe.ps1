[CmdletBinding()]
param(
    [string]$ArchiveRoot = 'C:\Windows\Temp\viogpu-pre-58063-archive',
    [string]$OutputDirectory = 'C:\Windows\Temp\viogpu-kmt-58063'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$devices = @(
    Get-PnpDevice -PresentOnly |
        Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' }
)
if ($devices.Count -ne 1) {
    throw "Expected exactly one present virtio-gpu device, found $($devices.Count)."
}

$driverProperty = Get-PnpDeviceProperty -InstanceId $devices[0].InstanceId -KeyName 'DEVPKEY_Device_Driver'
if ($null -eq $driverProperty.PSObject.Properties['Data'] -or
    [string]::IsNullOrWhiteSpace([string]$driverProperty.Data)) {
    throw "Device '$($devices[0].InstanceId)' has no driver registry key."
}
$driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($driverProperty.Data)"
$driverKey = Get-Item -LiteralPath $driverKeyPath
$driverVersion = [string]$driverKey.GetValue('DriverVersion', '')
if ($driverVersion -ne '100.6.101.58063') {
    throw "Expected active driver version 100.6.101.58063, got '$driverVersion'."
}

$clearedDiagnostics = @(
    $driverKey.GetValueNames() |
        Where-Object { $_ -like 'NativeContextCreate*' } |
        Sort-Object
)
foreach ($name in $clearedDiagnostics) {
    Remove-ItemProperty -LiteralPath $driverKeyPath -Name $name -Force
}

if (Test-Path -LiteralPath $OutputDirectory) {
    Remove-Item -LiteralPath $OutputDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$archiveDirectory = Join-Path $ArchiveRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
$archived = [System.Collections.Generic.List[string]]::new()

function Move-ToArchive([string]$Path, [string]$RelativeDestination) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $destination = Join-Path $archiveDirectory $RelativeDestination
    $parent = Split-Path -Parent $destination
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    Move-Item -LiteralPath $Path -Destination $destination -Force
    $archived.Add($Path)
}

Move-ToArchive 'C:\Windows\MEMORY.DMP' 'MEMORY.DMP'
Move-ToArchive 'C:\Windows\Temp\viogpu-kmt-58062' 'viogpu-kmt-58062'

if (Test-Path -LiteralPath 'C:\Windows\Minidump' -PathType Container) {
    foreach ($dump in Get-ChildItem -LiteralPath 'C:\Windows\Minidump' -File -Force) {
        Move-ToArchive $dump.FullName (Join-Path 'Minidump' $dump.Name)
    }
}

[pscustomobject]@{
    PreparedAt = (Get-Date).ToString('o')
    DriverVersion = $driverVersion
    DeviceStatus = [string]$devices[0].Status
    ClearedDiagnostics = $clearedDiagnostics
    ArchivedPaths = @($archived)
    ArchiveDirectory = if ($archived.Count) { $archiveDirectory } else { $null }
    OutputDirectory = $OutputDirectory
} | ConvertTo-Json -Depth 5
