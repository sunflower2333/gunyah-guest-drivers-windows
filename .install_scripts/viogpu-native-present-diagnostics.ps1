[CmdletBinding()]
param(
    [string]$DevicePattern = 'PCI\VEN_1AF4&DEV_1050*'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$devices = @(
    Get-PnpDevice -PresentOnly |
        Where-Object { $_.InstanceId -like $DevicePattern }
)
if ($devices.Count -ne 1) {
    throw "Expected one present virtio-gpu device matching '$DevicePattern', found $($devices.Count)."
}

$device = $devices[0]
$driverProperty = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
if ([string]::IsNullOrWhiteSpace([string]$driverProperty.Data)) {
    throw "Device '$($device.InstanceId)' has no DEVPKEY_Device_Driver value."
}

$driverKey = [string]$driverProperty.Data
$registryPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$driverKey"
$diagnostic = Get-ItemProperty -LiteralPath $registryPath
$valueNames = @(
    'NativePresentReason',
    'NativePresentStatus',
    'NativePresentContextType',
    'NativePresentFlags',
    'NativePresentSubRectCount',
    'NativePresentMultipassOffset',
    'NativePresentSourceFlags',
    'NativePresentDestinationFlags',
    'NativePresentSourceHostState',
    'NativePresentDestinationHostState',
    'NativePresentSourceResource2DState',
    'NativePresentDestinationResource2DState',
    'NativePresentSourcePlacementState',
    'NativePresentDestinationPlacementState',
    'NativePresentSourceFormat',
    'NativePresentDestinationFormat',
    'NativePresentSourceWidth',
    'NativePresentSourceHeight',
    'NativePresentSourcePitch',
    'NativePresentDestinationWidth',
    'NativePresentDestinationHeight',
    'NativePresentDestinationPitch',
    'NativePresentSourceAllocationListValue',
    'NativePresentDestinationAllocationListValue',
    'NativePresentSourceResourceId',
    'NativePresentDestinationResourceId',
    'NativePresentSourceRectLeft',
    'NativePresentSourceRectTop',
    'NativePresentSourceRectRight',
    'NativePresentSourceRectBottom',
    'NativePresentDestinationRectLeft',
    'NativePresentDestinationRectTop',
    'NativePresentDestinationRectRight',
    'NativePresentDestinationRectBottom'
)
foreach ($name in $valueNames) {
    if ($null -eq $diagnostic.PSObject.Properties[$name]) {
        throw "Driver key '$driverKey' does not contain a complete Present diagnostic; missing '$name'."
    }
}

function ConvertTo-DwordValue {
    param([object]$Value)

    [int64]$signed = [Convert]::ToInt64($Value)
    if ($signed -lt 0) {
        return [uint64]($signed + 0x100000000L)
    }
    return [uint64]$signed
}

function ConvertTo-SignedDwordValue {
    param([object]$Value)

    [uint64]$unsigned = ConvertTo-DwordValue $Value
    if ($unsigned -ge 0x80000000L) {
        return [int64]($unsigned - 0x100000000L)
    }
    return [int64]$unsigned
}

function Format-Dword {
    param([object]$Value)

    return '0x{0:X8}' -f (ConvertTo-DwordValue $Value)
}

function Decode-PlacementState {
    param([object]$Value)

    [uint64]$state = ConvertTo-DwordValue $Value
    $names = @()
    if (($state -band 0x01) -ne 0) { $names += 'PlacementValid' }
    if (($state -band 0x02) -ne 0) { $names += 'ApertureMdl' }
    if (($state -band 0x04) -ne 0) { $names += 'ApertureAddress' }
    if (($state -band 0x08) -ne 0) { $names += 'FullyMapped' }
    if (($state -band 0x10) -ne 0) { $names += 'Destroying' }
    if (($state -band 0x20) -ne 0) { $names += 'SignatureValid' }
    return $names -join ','
}

function Decode-AllocationListValue {
    param([object]$Value)

    [uint64]$bits = ConvertTo-DwordValue $Value
    return [pscustomobject]@{
        Value = ('0x{0:X8}' -f $bits)
        WriteOperation = (($bits -band 1) -ne 0)
        SegmentId = (($bits -shr 1) -band 0x1F)
        Reserved = (($bits -shr 6) -band 0x03FFFFFF)
    }
}

$reasonNames = @{
    1 = 'NativeSourceIdentity'
    2 = 'GdiSourcePlacement'
    3 = 'GdiSourceIdentity'
    4 = 'SourceObject'
    5 = 'DestinationObject'
    6 = 'SourcePlacement'
    7 = 'DestinationBacking'
    8 = 'DestinationPlacement'
    9 = 'Geometry'
    10 = 'SourcePrepatch'
    11 = 'DestinationPrepatch'
}

[uint64]$reason = ConvertTo-DwordValue $diagnostic.NativePresentReason
if ($reason -eq 0) {
    throw "The driver has not recorded a Present rejection since its current StartDevice entry."
}
$reasonName = $reasonNames[[int]$reason]
if ([string]::IsNullOrWhiteSpace($reasonName)) {
    $reasonName = 'Unknown'
}

[pscustomobject]@{
    InstanceId = $device.InstanceId
    DriverKey = $driverKey
    Reason = $reason
    ReasonName = $reasonName
    Status = Format-Dword $diagnostic.NativePresentStatus
    ContextType = ConvertTo-DwordValue $diagnostic.NativePresentContextType
    PresentFlags = Format-Dword $diagnostic.NativePresentFlags
    SubRectCount = ConvertTo-DwordValue $diagnostic.NativePresentSubRectCount
    MultipassOffset = ConvertTo-DwordValue $diagnostic.NativePresentMultipassOffset
    SourceFlags = Format-Dword $diagnostic.NativePresentSourceFlags
    DestinationFlags = Format-Dword $diagnostic.NativePresentDestinationFlags
    SourceHostState = ConvertTo-DwordValue $diagnostic.NativePresentSourceHostState
    DestinationHostState = ConvertTo-DwordValue $diagnostic.NativePresentDestinationHostState
    SourceResource2DState = ConvertTo-DwordValue $diagnostic.NativePresentSourceResource2DState
    DestinationResource2DState = ConvertTo-DwordValue $diagnostic.NativePresentDestinationResource2DState
    SourcePlacementState = Decode-PlacementState $diagnostic.NativePresentSourcePlacementState
    DestinationPlacementState = Decode-PlacementState $diagnostic.NativePresentDestinationPlacementState
    SourceFormat = Format-Dword $diagnostic.NativePresentSourceFormat
    DestinationFormat = Format-Dword $diagnostic.NativePresentDestinationFormat
    SourceSize = '{0}x{1}, pitch={2}' -f $diagnostic.NativePresentSourceWidth,
        $diagnostic.NativePresentSourceHeight, $diagnostic.NativePresentSourcePitch
    DestinationSize = '{0}x{1}, pitch={2}' -f $diagnostic.NativePresentDestinationWidth,
        $diagnostic.NativePresentDestinationHeight, $diagnostic.NativePresentDestinationPitch
    SourceAllocationList = Decode-AllocationListValue $diagnostic.NativePresentSourceAllocationListValue
    DestinationAllocationList = Decode-AllocationListValue $diagnostic.NativePresentDestinationAllocationListValue
    SourceResourceId = ConvertTo-DwordValue $diagnostic.NativePresentSourceResourceId
    DestinationResourceId = ConvertTo-DwordValue $diagnostic.NativePresentDestinationResourceId
    SourceRect = '{0},{1}-{2},{3}' -f (ConvertTo-SignedDwordValue $diagnostic.NativePresentSourceRectLeft),
        (ConvertTo-SignedDwordValue $diagnostic.NativePresentSourceRectTop),
        (ConvertTo-SignedDwordValue $diagnostic.NativePresentSourceRectRight),
        (ConvertTo-SignedDwordValue $diagnostic.NativePresentSourceRectBottom)
    DestinationRect = '{0},{1}-{2},{3}' -f (ConvertTo-SignedDwordValue $diagnostic.NativePresentDestinationRectLeft),
        (ConvertTo-SignedDwordValue $diagnostic.NativePresentDestinationRectTop),
        (ConvertTo-SignedDwordValue $diagnostic.NativePresentDestinationRectRight),
        (ConvertTo-SignedDwordValue $diagnostic.NativePresentDestinationRectBottom)
}
