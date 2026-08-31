[CmdletBinding()]
param(
    [string]$DevicePattern = 'PCI\VEN_1AF4&DEV_1050*',
    [Parameter(Mandatory = $true)]
    [uint32]$ContextId,
    [object]$DiagnosticData
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($null -eq $DiagnosticData) {
    $devices = @(
        Get-PnpDevice -PresentOnly |
            Where-Object { $_.InstanceId -like $DevicePattern }
    )
    if ($devices.Count -ne 1) {
        throw "Expected one present virtio-gpu device matching '$DevicePattern', found $($devices.Count)."
    }

    $driverProperty = Get-PnpDeviceProperty -InstanceId $devices[0].InstanceId -KeyName 'DEVPKEY_Device_Driver'
    $driverKey = [string]$driverProperty.Data
    if ([string]::IsNullOrWhiteSpace($driverKey)) {
        throw "Device '$($devices[0].InstanceId)' has no driver-key value."
    }
    $registryPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$driverKey"
    $DiagnosticData = Get-ItemProperty -LiteralPath $registryPath
}

$slot = [uint32]($ContextId % 64)
$valuePrefix = 'NativeContextDestroySlot{0:D2}' -f $slot

function Get-DiagnosticValue {
    param([string]$Suffix)

    $name = $valuePrefix + $Suffix
    $property = $DiagnosticData.PSObject.Properties[$name]
    if ($null -eq $property) {
        throw "Native Context destroy diagnostic is missing '$name'."
    }
    return $property.Value
}

function ConvertTo-DwordValue {
    param([object]$Value)

    [int64]$signed = [Convert]::ToInt64($Value)
    if ($signed -lt 0) {
        return [uint64]($signed + 0x100000000L)
    }
    return [uint64]$signed
}

function Format-Dword {
    param([object]$Value)

    return '0x{0:X8}' -f (ConvertTo-DwordValue $Value)
}

function Decode-Value {
    param(
        [object]$Value,
        [hashtable]$Names,
        [string]$Label,
        [switch]$RejectUnknown
    )

    [uint64]$number = ConvertTo-DwordValue $Value
    $name = $Names[(Format-Dword $number)]
    if ($null -eq $name) {
        if ($RejectUnknown) {
            throw "$Label value $(Format-Dword $number) is not recognized."
        }
        $name = 'Unknown'
    }
    return '{0} ({1})' -f $name, $number
}

$stageNames = @{
    '0x00000100' = 'Entered'
    '0x00000110' = 'Rundown'
    '0x00000120' = 'Busy'
    '0x00000130' = 'Adapter'
    '0x00000140' = 'Marked'
    '0x00000200' = 'HostBegin'
    '0x00000210' = 'HostResult'
    '0x00000300' = 'Retired'
    '0x00000FFF' = 'Complete'
}
$hostResultNames = @{
    '0x00000000' = 'NotSubmitted'
    '0x00000001' = 'Confirmed'
    '0x00000002' = 'Rejected'
    '0x00000003' = 'Unknown'
}
$contextStateNames = @{
    '0x00000000' = 'Allocated'
    '0x00000001' = 'Creating'
    '0x00000002' = 'Live'
    '0x00000003' = 'Destroying'
    '0x00000004' = 'Dead'
}
$ownerStateNames = @{
    '0x00000000' = 'Creating'
    '0x00000001' = 'Live'
    '0x00000002' = 'Destroying'
}

$storedContextId = ConvertTo-DwordValue (Get-DiagnosticValue 'ContextId')
if ($storedContextId -ne $ContextId) {
    throw "Native Context destroy slot $slot contains context $storedContextId, expected $ContextId."
}

$stage = ConvertTo-DwordValue (Get-DiagnosticValue 'Stage')
$hostResult = ConvertTo-DwordValue (Get-DiagnosticValue 'HostResult')
$contextState = ConvertTo-DwordValue (Get-DiagnosticValue 'ContextState')
$ownerState = ConvertTo-DwordValue (Get-DiagnosticValue 'OwnerState')

[pscustomobject]@{
    Slot = $slot
    Attempt = ConvertTo-DwordValue (Get-DiagnosticValue 'Attempt')
    Stage = Decode-Value $stage $stageNames 'Stage' -RejectUnknown
    Status = Format-Dword (Get-DiagnosticValue 'Status')
    Detail = Format-Dword (Get-DiagnosticValue 'Detail')
    HostResult = Decode-Value $hostResult $hostResultNames 'HostResult'
    ContextId = $storedContextId
    ContextState = Decode-Value $contextState $contextStateNames 'ContextState'
    OwnerState = Decode-Value $ownerState $ownerStateNames 'OwnerState'
    Released = (ConvertTo-DwordValue (Get-DiagnosticValue 'Released')) -ne 0
    Retrying = (ConvertTo-DwordValue (Get-DiagnosticValue 'Retrying')) -ne 0
    OwnerRetained = (ConvertTo-DwordValue (Get-DiagnosticValue 'OwnerRetained')) -ne 0
}
