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

foreach ($name in @('NativeStartStage', 'NativeStartStatus', 'NativeStartDetail')) {
    if ($null -eq $diagnostic.PSObject.Properties[$name]) {
        throw "Driver key '$driverKey' does not contain '$name'."
    }
}

$queryDiagnosticNames = @(
    'NativeQueryAdapterInfoType',
    'NativeQueryAdapterInfoStatus',
    'NativeQueryAdapterInfoInputSize',
    'NativeQueryAdapterInfoOutputSize'
)
$queryDiagnosticPresent = @(
    $queryDiagnosticNames | Where-Object { $null -ne $diagnostic.PSObject.Properties[$_] }
)
if ($queryDiagnosticPresent.Count -ne 0 -and $queryDiagnosticPresent.Count -ne $queryDiagnosticNames.Count) {
    throw "Driver key '$driverKey' contains a partial QueryAdapterInfo diagnostic."
}

$parameterDiagnosticNames = @(
    'NativeContextGetParamPhase',
    'NativeContextGetParamContextId',
    'NativeContextGetParamParameter',
    'NativeContextGetParamSequence',
    'NativeContextGetParamRequestCommand',
    'NativeContextGetParamRequestLength',
    'NativeContextGetParamRequestResponseOffset',
    'NativeContextGetParamRequestIoctlCommand',
    'NativeContextGetParamRequestPipe',
    'NativeContextGetParamRequestParameter',
    'NativeContextGetParamRequestValueLow',
    'NativeContextGetParamRequestValueHigh',
    'NativeContextGetParamRequestValueLength',
    'NativeContextGetParamRequestPadding',
    'NativeContextGetParamSeedAttempted',
    'NativeContextGetParamSeedWriteCompleted',
    'NativeContextGetParamSeedSharedSeqno',
    'NativeContextGetParamSeedSharedResponseOffset',
    'NativeContextGetParamSeedSharedAsyncError',
    'NativeContextGetParamSeedSharedGlobalFaults',
    'NativeContextGetParamSharedSeqno',
    'NativeContextGetParamSharedResponseOffset',
    'NativeContextGetParamSharedAsyncError',
    'NativeContextGetParamSharedGlobalFaults',
    'NativeContextGetParamCopyAttempted',
    'NativeContextGetParamCopyCompleted',
    'NativeContextGetParamInnerRet',
    'NativeContextGetParamInnerPipe',
    'NativeContextGetParamInnerParameter',
    'NativeContextGetParamInnerValueLow',
    'NativeContextGetParamInnerValueHigh',
    'NativeContextGetParamInnerValueLength',
    'NativeContextGetParamInnerPadding',
    'NativeContextGetParamOuterResponseSize',
    'NativeContextGetParamOuterType',
    'NativeContextGetParamOuterFlags',
    'NativeContextGetParamOuterFenceLow',
    'NativeContextGetParamOuterFenceHigh',
    'NativeContextGetParamOuterContextId',
    'NativeContextGetParamOuterRingIndex',
    'NativeContextGetParamOuterPadding',
    'NativeContextGetParamOuterSubmitted',
    'NativeContextGetParamOuterCompleted',
    'NativeContextGetParamOuterValidation',
    'NativeContextGetParamSubmitResult',
    'NativeContextGetParamValidation',
    'NativeContextGetParamResult'
)
$parameterDiagnosticPresent = @(
    $parameterDiagnosticNames | Where-Object { $null -ne $diagnostic.PSObject.Properties[$_] }
)
if ($parameterDiagnosticPresent.Count -ne 0 -and $parameterDiagnosticPresent.Count -ne $parameterDiagnosticNames.Count) {
    throw "Driver key '$driverKey' contains a partial GET_PARAM diagnostic."
}

$stageNames = @{
    0x0100 = 'Entered'
    0x0110 = 'Preconditions'
    0x0120 = 'DeviceInformation'
    0x0130 = 'HardwareIdentity'
    0x0140 = 'AdapterAllocation'
    0x0150 = 'RegistryConfiguration'
    0x0200 = 'BeginInitialization'
    0x0210 = 'PciResources'
    0x0300 = 'VirtioPreconditions'
    0x0310 = 'VirtioDevice'
    0x0320 = 'VirtioVersion'
    0x0330 = 'VirtioNativeFeatures'
    0x0340 = 'VirtioSetFeatures'
    0x0350 = 'VirtioFindQueues'
    0x0360 = 'VirtioQueueObjects'
    0x0370 = 'VirtioQueueBacklog'
    0x0380 = 'VirtioConfig'
    0x0400 = 'HostVisibleRegion'
    0x0410 = 'QueueBuffer'
    0x0420 = 'ResourceIds'
    0x0430 = 'QueueInterrupts'
    0x0440 = 'DriverReady'
    0x0450 = 'SynchronousRequests'
    0x0500 = 'CapsetFeatureState'
    0x0510 = 'CapsetCount'
    0x0520 = 'CapsetInfoQuery'
    0x0530 = 'CapsetInfoUnique'
    0x0540 = 'CapsetInfoLayout'
    0x0550 = 'CapsetPayloadQuery'
    0x0560 = 'CapsetPayloadValidation'
    0x0570 = 'CapsetPublish'
    0x0600 = 'ModeList'
    0x0610 = 'FrameSegment'
    0x0620 = 'CursorSegment'
    0x0700 = 'WorkThread'
    0x0710 = 'CompleteInitialization'
    0x0800 = 'HardwareInformation'
    0x0810 = 'PostDisplayOwnership'
    0x0820 = 'FinalState'
    0x0FFF = 'Complete'
}

$detailBits = [ordered]@{
    0x00000001 = 'MissingVirgl'
    0x00000002 = 'MissingResourceBlob'
    0x00000004 = 'MissingContextInit'
    0x00000008 = 'MissingGuestHandle'
    0x00000100 = 'InvalidWireVersion'
    0x00000200 = 'InvalidContextType'
    0x00000400 = 'InvalidPadding'
    0x00000800 = 'InvalidMsmVersion'
    0x00001000 = 'InvalidPriorities'
    0x00002000 = 'InvalidVaStart'
    0x00004000 = 'InvalidVaSize'
    0x00008000 = 'InvalidVaRange'
}

function ConvertTo-DwordValue {
    param([object]$Value)

    [int64]$signed = [Convert]::ToInt64($Value)
    if ($signed -lt 0) {
        return [uint64]($signed + 0x100000000L)
    }
    return [uint64]$signed
}

[uint64]$stage = ConvertTo-DwordValue $diagnostic.NativeStartStage
[uint64]$status = ConvertTo-DwordValue $diagnostic.NativeStartStatus
[uint64]$detail = ConvertTo-DwordValue $diagnostic.NativeStartDetail
$queryType = $null
$queryStatus = $null
$queryInputSize = $null
$queryOutputSize = $null
if ($queryDiagnosticPresent.Count -eq $queryDiagnosticNames.Count) {
    $queryType = '0x{0:X8}' -f (ConvertTo-DwordValue $diagnostic.NativeQueryAdapterInfoType)
    $queryStatus = '0x{0:X8}' -f (ConvertTo-DwordValue $diagnostic.NativeQueryAdapterInfoStatus)
    $queryInputSize = [uint64](ConvertTo-DwordValue $diagnostic.NativeQueryAdapterInfoInputSize)
    $queryOutputSize = [uint64](ConvertTo-DwordValue $diagnostic.NativeQueryAdapterInfoOutputSize)
}
$parameterSnapshot = $null
if ($parameterDiagnosticPresent.Count -eq $parameterDiagnosticNames.Count) {
    $parameterSnapshot = [ordered]@{}
    foreach ($name in $parameterDiagnosticNames) {
        $parameterSnapshot[$name] = ConvertTo-DwordValue $diagnostic.$name
    }
}
$stageName = $stageNames[[int]$stage]
if ([string]::IsNullOrWhiteSpace($stageName)) {
    $stageName = 'Unknown'
}

$decodedDetail = @()
if ($stage -eq 0x0330 -or $stage -eq 0x0500 -or $stage -eq 0x0560) {
    foreach ($entry in $detailBits.GetEnumerator()) {
        if (($detail -band [uint64]$entry.Key) -ne 0) {
            $decodedDetail += [string]$entry.Value
        }
    }
}

[pscustomobject]@{
    InstanceId = $device.InstanceId
    DriverKey = $driverKey
    Stage = ('0x{0:X4}' -f $stage)
    StageName = $stageName
    Status = ('0x{0:X8}' -f $status)
    Detail = ('0x{0:X8}' -f $detail)
    DetailMeaning = ($decodedDetail -join ',')
    NativeQueryAdapterInfoType = $queryType
    NativeQueryAdapterInfoStatus = $queryStatus
    NativeQueryAdapterInfoInputSize = $queryInputSize
    NativeQueryAdapterInfoOutputSize = $queryOutputSize
    NativeContextGetParam = $parameterSnapshot
}
