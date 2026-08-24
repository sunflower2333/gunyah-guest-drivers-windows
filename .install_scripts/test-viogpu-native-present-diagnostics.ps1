$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$names = @(
    'NativePresentReason',
    'NativePresentStatus',
    'NativePresentHardwareResetState',
    'NativePresentHardwareResetCallerRva',
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
$values['NativePresentReason'] = 18
$values['NativePresentExecuteStage'] = 19
$values['NativePresentExecuteStatus'] = -1073741661
$values['NativePresentExecuteDetail'] = 2
$values['NativePresentExecuteHardwareResetState'] = 1
$values['NativePresentExecuteHardwareResetCallerRva'] = 0x7880
$values['NativePresentExecuteSourcePlacementState'] = 0x2F
$values['NativePresentExecuteSourcePlacementOffsetLow'] = -1985229329
$values['NativePresentExecuteSourcePlacementOffsetHigh'] = 0x01234567

$result = & "$PSScriptRoot/viogpu-native-present-diagnostics.ps1" `
    -DiagnosticData ([pscustomobject]$values)
if ($result.ExecuteStage -ne 'HostPresent (19)' -or
    $result.ExecuteStatus -ne '0xC00000A3' -or
    $result.ExecuteHardwareResetState -ne 'ResetRequested (1)' -or
    $result.ExecuteHardwareResetCallerRva -ne '0x00007880' -or
    $result.ExecuteSourcePlacementState -ne 'PlacementValid,ApertureMdl,ApertureAddress,FullyMapped,SignatureValid' -or
    $result.ExecuteSourcePlacementOffset -ne '0x0123456789ABCDEF') {
    throw "Native Present diagnostic decoder fixture failed: $($result | Format-List | Out-String)"
}

$presentOnlyValues = [ordered]@{}
foreach ($name in $names | Where-Object { $_ -notlike 'NativePresentExecute*' }) {
    $presentOnlyValues[$name] = 0
}
$presentOnlyValues['NativePresentReason'] = 18
$presentOnlyValues['NativePresentExecuteStage'] = 0
$presentOnly = & "$PSScriptRoot/viogpu-native-present-diagnostics.ps1" `
    -DiagnosticData ([pscustomobject]$presentOnlyValues)
if ($presentOnly.ReasonName -ne 'TransactionRegistration' -or
    $null -ne $presentOnly.PSObject.Properties['ExecuteStage']) {
    throw "Present-only diagnostic decoder fixture failed: $($presentOnly | Format-List | Out-String)"
}

$executionOnlyValues = [ordered]@{ NativePresentReason = 0 }
foreach ($name in $names | Where-Object { $_ -like 'NativePresentExecute*' }) {
    $executionOnlyValues[$name] = 0
}
$executionOnlyValues['NativePresentExecuteStage'] = 19
$executionOnlyValues['NativePresentExecuteStatus'] = -1073741661
$executionOnly = & "$PSScriptRoot/viogpu-native-present-diagnostics.ps1" `
    -DiagnosticData ([pscustomobject]$executionOnlyValues)
if ($executionOnly.ExecuteStage -ne 'HostPresent (19)' -or
    $null -ne $executionOnly.PSObject.Properties['Reason']) {
    throw "Execution-only diagnostic decoder fixture failed: $($executionOnly | Format-List | Out-String)"
}

Write-Host 'Native Present diagnostic decoder fixture: PASS'
