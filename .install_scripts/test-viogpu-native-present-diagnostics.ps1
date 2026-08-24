$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$names = @(
    'NativePresentDiagnosticEpoch',
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
    'NativePresentDestinationRectBottom',
    'NativePresentExecuteStage',
    'NativePresentExecuteStatus',
    'NativePresentExecuteDetail',
    'NativePresentExecuteResetProvenanceValid',
    'NativePresentExecuteHardwareResetState',
    'NativePresentExecuteHardwareResetCallerRva',
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

$values = [ordered]@{}
foreach ($name in $names) {
    $values[$name] = 0
}
$values['NativePresentDiagnosticEpoch'] = 2
$values['NativePresentReason'] = 18
$values['NativePresentSubmissionFaultProvenanceValid'] = 1
$values['NativePresentSubmissionFaultCallerRva'] = 0x4321
$values['NativePresentSubmissionFaultExecutionDiagnosticState'] = 2
$values['NativePresentSubmissionFaultPresentSubmitStage'] = 7
$values['NativePresentSubmissionFaultPresentSubmitStatus'] = -1073741661
$values['NativePresentSubmissionFaultPresentSubmitDetail'] = 0x030201
$values['NativePresentExecuteStage'] = 19
$values['NativePresentExecuteStatus'] = -1073741661
$values['NativePresentExecuteDetail'] = 2
$values['NativePresentExecuteResetProvenanceValid'] = 1
$values['NativePresentExecuteHardwareResetState'] = 1
$values['NativePresentExecuteHardwareResetCallerRva'] = 0x7880
$values['NativePresentExecuteSourcePlacementState'] = 0x2F
$values['NativePresentExecuteSourcePlacementOffsetLow'] = -1985229329
$values['NativePresentExecuteSourcePlacementOffsetHigh'] = 0x01234567

$result = & "$PSScriptRoot/viogpu-native-present-diagnostics.ps1" `
    -DiagnosticData ([pscustomobject]$values)
if ($result.ExecuteStage -ne 'HostPresent (19)' -or
    $result.ExecuteStatus -ne '0xC00000A3' -or
    -not $result.ExecuteResetProvenanceAvailable -or
    $result.ExecuteHardwareResetState -ne 'ResetRequested (1)' -or
    $result.ExecuteHardwareResetCallerRva -ne '0x00007880' -or
    -not $result.SubmissionFaultProvenanceAvailable -or
    $result.SubmissionFaultCallerRva -ne '0x00004321' -or
    $result.SubmissionFaultExecutionDiagnosticState -ne 'Consumed (2)' -or
    $result.SubmissionFaultPresentSubmitStage -ne 'PassiveQueue (7)' -or
    $result.SubmissionFaultPresentSubmitStatus -ne '0xC00000A3' -or
    $result.SubmissionFaultPresentSubmitDetail -ne '0x00030201' -or
    $result.ExecuteSourcePlacementState -ne 'PlacementValid,ApertureMdl,ApertureAddress,FullyMapped,SignatureValid' -or
    $result.ExecuteSourcePlacementOffset -ne '0x0123456789ABCDEF') {
    throw "Native Present diagnostic decoder fixture failed: $($result | Format-List | Out-String)"
}

$presentOnlyValues = [ordered]@{}
foreach ($name in $names | Where-Object { $_ -notlike 'NativePresentExecute*' }) {
    $presentOnlyValues[$name] = 0
}
$presentOnlyValues['NativePresentReason'] = 18
$presentOnlyValues['NativePresentDiagnosticEpoch'] = 2
$presentOnlyValues['NativePresentExecuteStage'] = 0
$presentOnly = & "$PSScriptRoot/viogpu-native-present-diagnostics.ps1" `
    -DiagnosticData ([pscustomobject]$presentOnlyValues)
if ($presentOnly.ReasonName -ne 'TransactionRegistration' -or
    $null -ne $presentOnly.PSObject.Properties['ExecuteStage']) {
    throw "Present-only diagnostic decoder fixture failed: $($presentOnly | Format-List | Out-String)"
}

$executionOnlyValues = [ordered]@{
    NativePresentDiagnosticEpoch = 2
    NativePresentReason = 0
}
foreach ($name in $names | Where-Object {
        $_ -like 'NativePresentExecute*' -and
        $_ -notin @('NativePresentExecuteHardwareResetState', 'NativePresentExecuteHardwareResetCallerRva')
    }) {
    $executionOnlyValues[$name] = 0
}
$executionOnlyValues['NativePresentExecuteStage'] = 19
$executionOnlyValues['NativePresentExecuteStatus'] = -1073741661
$executionOnly = & "$PSScriptRoot/viogpu-native-present-diagnostics.ps1" `
    -DiagnosticData ([pscustomobject]$executionOnlyValues)
if ($executionOnly.ExecuteStage -ne 'HostPresent (19)' -or
    $executionOnly.ExecuteResetProvenanceAvailable -or
    $null -ne $executionOnly.PSObject.Properties['ExecuteHardwareResetState'] -or
    $null -ne $executionOnly.PSObject.Properties['Reason']) {
    throw "Execution-only diagnostic decoder fixture failed: $($executionOnly | Format-List | Out-String)"
}

$stateTransitionValues = [ordered]@{}
foreach ($entry in $executionOnlyValues.GetEnumerator()) {
    $stateTransitionValues[$entry.Key] = $entry.Value
}
$stateTransitionValues['NativePresentExecuteStage'] = 22
$stateTransitionValues['NativePresentExecuteDetail'] = 6
$stateTransition = & "$PSScriptRoot/viogpu-native-present-diagnostics.ps1" `
    -DiagnosticData ([pscustomobject]$stateTransitionValues)
if ($stateTransition.ExecuteStage -ne 'StateTransition (22)' -or
    $stateTransition.ExecuteDetail -ne '0x00000006') {
    throw "State-transition diagnostic decoder fixture failed: $($stateTransition | Format-List | Out-String)"
}

$staleEpochValues = [ordered]@{}
foreach ($entry in $values.GetEnumerator()) {
    $staleEpochValues[$entry.Key] = $entry.Value
}
$staleEpochValues['NativePresentDiagnosticEpoch'] = 3
$staleEpochRejected = $false
try {
    $null = & "$PSScriptRoot/viogpu-native-present-diagnostics.ps1" `
        -DiagnosticData ([pscustomobject]$staleEpochValues)
}
catch {
    $staleEpochRejected = $_.Exception.Message -like '*incomplete Present diagnostic epoch*'
}
if (-not $staleEpochRejected) {
    throw 'Stale Present diagnostic epoch was not rejected.'
}

$racingMarkerFence = [pscustomobject]@{
    EpochBefore = 2
    EpochAfter = 4
    ReasonBefore = 18
    ReasonAfter = 18
    ExecuteStageBefore = 19
    ExecuteStageAfter = 19
}
$racingReadRejected = $false
try {
    $null = & "$PSScriptRoot/viogpu-native-present-diagnostics.ps1" `
        -DiagnosticData ([pscustomobject]$values) `
        -DiagnosticReadFence $racingMarkerFence
}
catch {
    $racingReadRejected = $_.Exception.Message -like '*changed or has an incomplete Present diagnostic epoch*'
}
if (-not $racingReadRejected) {
    throw 'Racing Present diagnostic marker snapshot was not rejected.'
}

Write-Host 'Native Present diagnostic decoder fixture: PASS'
