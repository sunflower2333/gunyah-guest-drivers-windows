[CmdletBinding()]
param(
    [string]$DevicePattern = 'PCI\VEN_1AF4&DEV_1050*',
    [object]$DiagnosticData,
    [object]$DiagnosticReadFence
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

    $device = $devices[0]
    $driverProperty = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
    if ([string]::IsNullOrWhiteSpace([string]$driverProperty.Data)) {
        throw "Device '$($device.InstanceId)' has no DEVPKEY_Device_Driver value."
    }

    $driverKey = [string]$driverProperty.Data
    $registryPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$driverKey"
    $epochBefore = Get-ItemPropertyValue -LiteralPath $registryPath -Name 'NativePresentDiagnosticEpoch'
    $reasonBefore = Get-ItemPropertyValue -LiteralPath $registryPath -Name 'NativePresentReason'
    $executeStageBefore = Get-ItemPropertyValue -LiteralPath $registryPath -Name 'NativePresentExecuteStage'
    $diagnostic = Get-ItemProperty -LiteralPath $registryPath
    $executeStageAfter = Get-ItemPropertyValue -LiteralPath $registryPath -Name 'NativePresentExecuteStage'
    $reasonAfter = Get-ItemPropertyValue -LiteralPath $registryPath -Name 'NativePresentReason'
    $epochAfter = Get-ItemPropertyValue -LiteralPath $registryPath -Name 'NativePresentDiagnosticEpoch'
    $markerFence = [pscustomobject]@{
        EpochBefore = $epochBefore
        EpochAfter = $epochAfter
        ReasonBefore = $reasonBefore
        ReasonAfter = $reasonAfter
        ExecuteStageBefore = $executeStageBefore
        ExecuteStageAfter = $executeStageAfter
    }
}
else {
    $device = [pscustomobject]@{ InstanceId = 'DiagnosticFixture' }
    $driverKey = 'DiagnosticFixture'
    $diagnostic = $DiagnosticData
    $markerFence = $DiagnosticReadFence
}
$valueNames = @(
    'NativePresentReason',
    'NativePresentStatus',
    'NativePresentHardwareResetState',
    'NativePresentHardwareResetCallerRva',
    'NativePresentSubmissionFaultProvenanceValid',
    'NativePresentSubmissionFaultCallerRva',
    'NativePresentSubmissionFaultExecutionDiagnosticState',
    'NativePresentSubmissionFaultPresentSubmitStage',
    'NativePresentSubmissionFaultPresentSubmitStatus',
    'NativePresentSubmissionFaultPresentSubmitDetail',
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
$executionValueNames = @(
    'NativePresentExecuteStage',
    'NativePresentExecuteStatus',
    'NativePresentExecuteDetail',
    'NativePresentExecuteResetProvenanceValid',
    'NativePresentExecuteFenceId',
    'NativePresentExecuteTransactionState',
    'NativePresentExecuteContextType',
    'NativePresentExecuteSourceResourceId',
    'NativePresentExecuteDestinationResourceId',
    'NativePresentExecuteSourcePlacementState',
    'NativePresentExecuteDestinationPlacementState',
    'NativePresentExecuteSourceResource2DState',
    'NativePresentExecuteDestinationResource2DState',
    'NativePresentExecuteSourcePlacementOffsetLow',
    'NativePresentExecuteSourcePlacementOffsetHigh',
    'NativePresentExecuteDestinationPlacementOffsetLow',
    'NativePresentExecuteDestinationPlacementOffsetHigh',
    'NativePresentExecuteTransactionSourcePlacementOffsetLow',
    'NativePresentExecuteTransactionSourcePlacementOffsetHigh',
    'NativePresentExecuteTransactionDestinationPlacementOffsetLow',
    'NativePresentExecuteTransactionDestinationPlacementOffsetHigh',
    'NativePresentExecuteSourceResetGenerationLow',
    'NativePresentExecuteSourceResetGenerationHigh',
    'NativePresentExecuteDestinationResetGenerationLow',
    'NativePresentExecuteDestinationResetGenerationHigh',
    'NativePresentExecuteTransactionDestinationResetGenerationLow',
    'NativePresentExecuteTransactionDestinationResetGenerationHigh'
)
$executionResetProvenanceValueNames = @(
    'NativePresentExecuteHardwareResetState',
    'NativePresentExecuteHardwareResetCallerRva'
)
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

function Format-QwordParts {
    param(
        [object]$Low,
        [object]$High
    )

    [uint64]$lowValue = ConvertTo-DwordValue $Low
    [uint64]$highValue = ConvertTo-DwordValue $High
    return '0x{0:X16}' -f (($highValue -shl 32) -bor $lowValue)
}

function Decode-HardwareResetState {
    param([object]$Value)

    [uint64]$state = ConvertTo-DwordValue $Value
    $name = switch ($state) {
        0 { 'Active' }
        1 { 'ResetRequested' }
        2 { 'Recovering' }
        default { 'Unknown' }
    }
    return '{0} ({1})' -f $name, $state
}

function Decode-PresentExecutionDiagnosticState {
    param([object]$Value)

    [uint64]$state = ConvertTo-DwordValue $Value
    $name = switch ($state) {
        0 { 'Open' }
        1 { 'Claimed' }
        2 { 'Consumed' }
        default { 'Unknown' }
    }
    return '{0} ({1})' -f $name, $state
}

function Decode-PresentSubmitStage {
    param([object]$Value)

    [uint64]$stage = ConvertTo-DwordValue $Value
    $name = switch ($stage) {
        0 { 'None' }
        1 { 'ResolveTransaction' }
        2 { 'Contract' }
        3 { 'PrepatchTransition' }
        4 { 'Cancelled' }
        5 { 'WorkReference' }
        6 { 'QueueTransition' }
        7 { 'PassiveQueue' }
        0x0FFF { 'Unexpected' }
        default { 'Unknown' }
    }
    return '{0} ({1})' -f $name, $stage
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

function Decode-PresentExecutionStage {
    param([object]$Value)

    [uint64]$stage = ConvertTo-DwordValue $Value
    $name = switch ($stage) {
        0 { 'None' }
        1 { 'InvalidTransaction' }
        2 { 'SourceLifecycle' }
        3 { 'DestinationLifecycle' }
        4 { 'GdiSourceReconcile' }
        5 { 'SourceIdentity' }
        6 { 'SourceObject' }
        7 { 'DestinationObject' }
        8 { 'AliasedAllocations' }
        9 { 'DestinationPrimary' }
        10 { 'SourcePlacement' }
        11 { 'DestinationBacking' }
        12 { 'DestinationPlacement' }
        13 { 'Geometry' }
        14 { 'SourcePlacementOffset' }
        15 { 'DestinationPlacementOffset' }
        16 { 'DestinationResetGeneration' }
        17 { 'CopyAddress' }
        18 { 'Cancelled' }
        19 { 'HostPresent' }
        20 { 'SubmissionOperation' }
        21 { 'TransactionRetire' }
        22 { 'StateTransition' }
        0x0FFF { 'Complete' }
        default { 'Unknown' }
    }
    return '{0} ({1})' -f $name, $stage
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
    12 = 'ContextReference'
    13 = 'SourceReference'
    14 = 'DestinationReference'
    15 = 'SourceLifecycle'
    16 = 'DestinationLifecycle'
    17 = 'TransactionReference'
    18 = 'TransactionRegistration'
    19 = 'ContextPublication'
    20 = 'EntryRejected'
}

# Stages recorded by RecordNative2DBackingDiagnostic when a standard 2D
# allocation cannot be backed.  A present that finds its source unbacked
# reports only Resource2DState = 0 for all four causes; these separate them.
$backingStages = @{
    1 = 'Reconcile'
    2 = 'Placement'
    3 = 'Format'
    4 = 'ApertureEntries'
    5 = 'HostCreateOrAttach'
}

foreach ($marker in @('NativePresentDiagnosticEpoch', 'NativePresentReason', 'NativePresentExecuteStage')) {
    if ($null -eq $diagnostic.PSObject.Properties[$marker]) {
        throw "Driver key '$driverKey' does not contain the Present diagnostic marker '$marker'."
    }
}

if ($null -eq $markerFence) {
    $markerFence = [pscustomobject]@{
        EpochBefore = $diagnostic.NativePresentDiagnosticEpoch
        EpochAfter = $diagnostic.NativePresentDiagnosticEpoch
        ReasonBefore = $diagnostic.NativePresentReason
        ReasonAfter = $diagnostic.NativePresentReason
        ExecuteStageBefore = $diagnostic.NativePresentExecuteStage
        ExecuteStageAfter = $diagnostic.NativePresentExecuteStage
    }
}
[uint64]$epoch = ConvertTo-DwordValue $diagnostic.NativePresentDiagnosticEpoch
[uint64]$reason = ConvertTo-DwordValue $diagnostic.NativePresentReason
[uint64]$executeStage = ConvertTo-DwordValue $diagnostic.NativePresentExecuteStage
[uint64]$epochBefore = ConvertTo-DwordValue $markerFence.EpochBefore
[uint64]$epochAfter = ConvertTo-DwordValue $markerFence.EpochAfter
[uint64]$reasonBefore = ConvertTo-DwordValue $markerFence.ReasonBefore
[uint64]$reasonAfter = ConvertTo-DwordValue $markerFence.ReasonAfter
[uint64]$executeStageBefore = ConvertTo-DwordValue $markerFence.ExecuteStageBefore
[uint64]$executeStageAfter = ConvertTo-DwordValue $markerFence.ExecuteStageAfter
if ($epoch -eq 0 -or ($epoch -band 1) -ne 0 -or
    $epochBefore -ne $epoch -or $epochAfter -ne $epoch -or
    $reasonBefore -ne $reason -or $reasonAfter -ne $reason -or
    $executeStageBefore -ne $executeStage -or $executeStageAfter -ne $executeStage) {
    throw "Driver key '$driverKey' changed or has an incomplete Present diagnostic epoch."
}
if ($reason -eq 0 -and $executeStage -eq 0) {
    throw "The driver has not committed a Present diagnostic since its current StartDevice entry."
}
if ($reason -ne 0) {
    foreach ($name in $valueNames) {
        if ($null -eq $diagnostic.PSObject.Properties[$name]) {
            throw "Driver key '$driverKey' contains an incomplete Present rejection; missing '$name'."
        }
    }
}
$submissionFaultProvenanceAvailable = $false
if ($reason -ne 0) {
    [uint64]$submissionFaultProvenanceValid =
        ConvertTo-DwordValue $diagnostic.NativePresentSubmissionFaultProvenanceValid
    if ($submissionFaultProvenanceValid -gt 1) {
        throw "Driver key '$driverKey' contains an invalid submission-fault provenance marker."
    }
    $submissionFaultProvenanceAvailable = $submissionFaultProvenanceValid -eq 1
    if ($submissionFaultProvenanceAvailable) {
        [uint64]$submissionFaultExecutionDiagnosticState =
            ConvertTo-DwordValue $diagnostic.NativePresentSubmissionFaultExecutionDiagnosticState
        if ($submissionFaultExecutionDiagnosticState -gt 2) {
            throw "Driver key '$driverKey' contains an invalid submission-fault execution diagnostic state."
        }
        [uint64]$submissionFaultPresentSubmitStage =
            ConvertTo-DwordValue $diagnostic.NativePresentSubmissionFaultPresentSubmitStage
        if ($submissionFaultPresentSubmitStage -notin @(0, 1, 2, 3, 4, 5, 6, 7, 0x0FFF)) {
            throw "Driver key '$driverKey' contains an invalid Present Submit stage."
        }
    }
}
if ($executeStage -ne 0) {
    foreach ($name in $executionValueNames) {
        if ($null -eq $diagnostic.PSObject.Properties[$name]) {
            throw "Driver key '$driverKey' contains an incomplete Present execution diagnostic; missing '$name'."
        }
    }
}
$resetProvenanceAvailable = $false
if ($executeStage -ne 0) {
    [uint64]$resetProvenanceValid = ConvertTo-DwordValue $diagnostic.NativePresentExecuteResetProvenanceValid
    if ($resetProvenanceValid -gt 1) {
        throw "Driver key '$driverKey' contains an invalid Present execution reset provenance marker."
    }
    $resetProvenanceAvailable = $resetProvenanceValid -eq 1
    if ($resetProvenanceAvailable) {
        foreach ($name in $executionResetProvenanceValueNames) {
            if ($null -eq $diagnostic.PSObject.Properties[$name]) {
                throw "Driver key '$driverKey' contains incomplete Present execution reset provenance; missing '$name'."
            }
        }
    }
}
$reasonName = $reasonNames[[int]$reason]
if ([string]::IsNullOrWhiteSpace($reasonName)) {
    $reasonName = 'Unknown'
}

$result = [ordered]@{
    InstanceId = $device.InstanceId
    DriverKey = $driverKey
}
if ($reason -ne 0) {
    $presentResult = [ordered]@{
    Reason = $reason
    ReasonName = $reasonName
    Status = Format-Dword $diagnostic.NativePresentStatus
    HardwareResetState = Decode-HardwareResetState $diagnostic.NativePresentHardwareResetState
    HardwareResetCallerRva = Format-Dword $diagnostic.NativePresentHardwareResetCallerRva
    SubmissionFaultProvenanceAvailable = $submissionFaultProvenanceAvailable
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
    if ($submissionFaultProvenanceAvailable) {
        $presentResult['SubmissionFaultCallerRva'] = Format-Dword `
            $diagnostic.NativePresentSubmissionFaultCallerRva
        $presentResult['SubmissionFaultExecutionDiagnosticState'] = Decode-PresentExecutionDiagnosticState `
            $diagnostic.NativePresentSubmissionFaultExecutionDiagnosticState
        $presentResult['SubmissionFaultPresentSubmitStage'] = Decode-PresentSubmitStage `
            $diagnostic.NativePresentSubmissionFaultPresentSubmitStage
        $presentResult['SubmissionFaultPresentSubmitStatus'] = Format-Dword `
            $diagnostic.NativePresentSubmissionFaultPresentSubmitStatus
        $presentResult['SubmissionFaultPresentSubmitDetail'] = Format-Dword `
            $diagnostic.NativePresentSubmissionFaultPresentSubmitDetail
    }
    foreach ($entry in $presentResult.GetEnumerator()) {
        $result[$entry.Key] = $entry.Value
    }
}
if ($executeStage -ne 0) {
    $executionResult = [ordered]@{
    ExecuteStage = Decode-PresentExecutionStage $diagnostic.NativePresentExecuteStage
    ExecuteStatus = Format-Dword $diagnostic.NativePresentExecuteStatus
    ExecuteDetail = Format-Dword $diagnostic.NativePresentExecuteDetail
    ExecuteResetProvenanceAvailable = $resetProvenanceAvailable
    ExecuteFenceId = ConvertTo-DwordValue $diagnostic.NativePresentExecuteFenceId
    ExecuteTransactionState = ConvertTo-DwordValue $diagnostic.NativePresentExecuteTransactionState
    ExecuteContextType = ConvertTo-DwordValue $diagnostic.NativePresentExecuteContextType
    ExecuteSourceResourceId = ConvertTo-DwordValue $diagnostic.NativePresentExecuteSourceResourceId
    ExecuteDestinationResourceId = ConvertTo-DwordValue $diagnostic.NativePresentExecuteDestinationResourceId
    ExecuteSourcePlacementState = Decode-PlacementState $diagnostic.NativePresentExecuteSourcePlacementState
    ExecuteDestinationPlacementState = Decode-PlacementState $diagnostic.NativePresentExecuteDestinationPlacementState
    ExecuteSourceResource2DState = ConvertTo-DwordValue $diagnostic.NativePresentExecuteSourceResource2DState
    ExecuteDestinationResource2DState = ConvertTo-DwordValue $diagnostic.NativePresentExecuteDestinationResource2DState
    ExecuteSourcePlacementOffset = Format-QwordParts `
        $diagnostic.NativePresentExecuteSourcePlacementOffsetLow `
        $diagnostic.NativePresentExecuteSourcePlacementOffsetHigh
    ExecuteDestinationPlacementOffset = Format-QwordParts `
        $diagnostic.NativePresentExecuteDestinationPlacementOffsetLow `
        $diagnostic.NativePresentExecuteDestinationPlacementOffsetHigh
    ExecuteTransactionSourcePlacementOffset = Format-QwordParts `
        $diagnostic.NativePresentExecuteTransactionSourcePlacementOffsetLow `
        $diagnostic.NativePresentExecuteTransactionSourcePlacementOffsetHigh
    ExecuteTransactionDestinationPlacementOffset = Format-QwordParts `
        $diagnostic.NativePresentExecuteTransactionDestinationPlacementOffsetLow `
        $diagnostic.NativePresentExecuteTransactionDestinationPlacementOffsetHigh
    ExecuteSourceResetGeneration = Format-QwordParts `
        $diagnostic.NativePresentExecuteSourceResetGenerationLow `
        $diagnostic.NativePresentExecuteSourceResetGenerationHigh
    ExecuteDestinationResetGeneration = Format-QwordParts `
        $diagnostic.NativePresentExecuteDestinationResetGenerationLow `
        $diagnostic.NativePresentExecuteDestinationResetGenerationHigh
    ExecuteTransactionDestinationResetGeneration = Format-QwordParts `
        $diagnostic.NativePresentExecuteTransactionDestinationResetGenerationLow `
        $diagnostic.NativePresentExecuteTransactionDestinationResetGenerationHigh
    }
    if ($resetProvenanceAvailable) {
        $executionResult['ExecuteHardwareResetState'] = Decode-HardwareResetState `
            $diagnostic.NativePresentExecuteHardwareResetState
        $executionResult['ExecuteHardwareResetCallerRva'] = Format-Dword `
            $diagnostic.NativePresentExecuteHardwareResetCallerRva
    }
    foreach ($entry in $executionResult.GetEnumerator()) {
        $result[$entry.Key] = $entry.Value
    }
}
[pscustomobject]$result
