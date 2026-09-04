[CmdletBinding()]
param(
    [int]$DiskNumber = 1,
    [int]$PartitionNumber = 3,
    [char]$DriveLetter = 'D'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$systemPartition = Get-Partition | Where-Object IsSystem
if ($systemPartition.DiskNumber -eq $DiskNumber) {
    throw "Refusing to modify system disk $DiskNumber."
}

$disk = Get-Disk -Number $DiskNumber
if ($disk.PartitionStyle -ne 'GPT' -or $disk.Size -ne 42949672960) {
    throw "Unexpected analysis disk identity: style=$($disk.PartitionStyle) size=$($disk.Size)."
}

if ($disk.IsReadOnly) {
    Set-Disk -Number $DiskNumber -IsReadOnly $false
}
if ($disk.IsOffline) {
    Set-Disk -Number $DiskNumber -IsOffline $false
}

$partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber
$expectedAccessPath = "${DriveLetter}:\"
if ($partition.AccessPaths -notcontains $expectedAccessPath) {
    Add-PartitionAccessPath -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -AccessPath $expectedAccessPath
}

$dumpCandidates = @(
    "${DriveLetter}:\Windows\MEMORY.DMP"
)
$minidumpDirectory = "${DriveLetter}:\Windows\Minidump"
if (Test-Path -LiteralPath $minidumpDirectory -PathType Container) {
    $dumpCandidates += @(
        Get-ChildItem -LiteralPath $minidumpDirectory -File -Filter '*.dmp' |
            Select-Object -ExpandProperty FullName
    )
}

[pscustomobject]@{
    MountedAt = (Get-Date).ToString('o')
    Disk = Get-Disk -Number $DiskNumber |
        Select-Object Number, PartitionStyle, OperationalStatus, HealthStatus, IsOffline, IsReadOnly, Size
    Partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber |
        Select-Object DiskNumber, PartitionNumber, DriveLetter, Type, Size, AccessPaths
    DumpCandidates = @(
        foreach ($path in $dumpCandidates) {
            if (Test-Path -LiteralPath $path -PathType Leaf) {
                Get-Item -LiteralPath $path |
                    Select-Object FullName, Length, CreationTimeUtc, LastWriteTimeUtc
            }
        }
    )
} | ConvertTo-Json -Depth 5
