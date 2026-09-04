[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$disks = @(Get-Disk | Sort-Object Number)
$partitions = @(Get-Partition | Sort-Object DiskNumber, PartitionNumber)
$volumes = @(Get-Volume | Sort-Object DriveLetter, FileSystemLabel)

[pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    Disks = @(
        $disks | Select-Object Number, FriendlyName, SerialNumber, PartitionStyle,
            OperationalStatus, HealthStatus, IsOffline, IsReadOnly, Size, UniqueId, Location
    )
    Partitions = @(
        $partitions | Select-Object DiskNumber, PartitionNumber, DriveLetter, Type, Size,
            IsActive, IsBoot, IsSystem, IsHidden, IsReadOnly, AccessPaths
    )
    Volumes = @(
        $volumes | Select-Object DriveLetter, FileSystemLabel, FileSystem, DriveType,
            HealthStatus, OperationalStatus, Size, SizeRemaining, Path
    )
} | ConvertTo-Json -Depth 6
