$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$values = [ordered]@{
    NativeContextDestroySlot02Attempt = 7
    NativeContextDestroySlot02Stage = 0x0210
    NativeContextDestroySlot02Status = -1073741661
    NativeContextDestroySlot02Detail = 0x00000000
    NativeContextDestroySlot02HostResult = 3
    NativeContextDestroySlot02ContextId = 2
    NativeContextDestroySlot02ContextState = 3
    NativeContextDestroySlot02OwnerState = 2
    NativeContextDestroySlot02Released = 0
    NativeContextDestroySlot02Retrying = 1
    NativeContextDestroySlot02OwnerRetained = 1
}
$busy = & "$PSScriptRoot/viogpu-native-context-destroy-diagnostics.ps1" `
    -ContextId 2 -DiagnosticData ([pscustomobject]$values)
if ($busy.Slot -ne 2 -or
    $busy.Attempt -ne 7 -or
    $busy.Stage -ne 'HostResult (528)' -or
    $busy.Status -ne '0xC00000A3' -or
    $busy.HostResult -ne 'Unknown (3)' -or
    $busy.ContextId -ne 2 -or
    $busy.ContextState -ne 'Destroying (3)' -or
    $busy.OwnerState -ne 'Destroying (2)' -or
    $busy.Released -or
    -not $busy.Retrying -or
    -not $busy.OwnerRetained) {
    throw "Retained Host-cleanup diagnostic fixture failed: $($busy | Format-List | Out-String)"
}

$values['NativeContextDestroySlot02Attempt'] = 12
$values['NativeContextDestroySlot02Stage'] = 0x0FFF
$values['NativeContextDestroySlot02Status'] = 0
$values['NativeContextDestroySlot02HostResult'] = 1
$values['NativeContextDestroySlot02ContextState'] = 4
$values['NativeContextDestroySlot02Released'] = 1
$values['NativeContextDestroySlot02Retrying'] = 0
$values['NativeContextDestroySlot02OwnerRetained'] = 0
$complete = & "$PSScriptRoot/viogpu-native-context-destroy-diagnostics.ps1" `
    -ContextId 2 -DiagnosticData ([pscustomobject]$values)
if ($complete.Stage -ne 'Complete (4095)' -or
    $complete.Status -ne '0x00000000' -or
    $complete.HostResult -ne 'Confirmed (1)' -or
    $complete.ContextState -ne 'Dead (4)' -or
    -not $complete.Released -or
    $complete.Retrying -or
    $complete.OwnerRetained) {
    throw "Released Native Context diagnostic fixture failed: $($complete | Format-List | Out-String)"
}

$collisionRejected = $false
try {
    & "$PSScriptRoot/viogpu-native-context-destroy-diagnostics.ps1" `
        -ContextId 66 -DiagnosticData ([pscustomobject]$values) | Out-Null
} catch {
    $collisionRejected = $_.Exception.Message -like '*contains context 2, expected 66*'
}
if (-not $collisionRejected) {
    throw 'Native Context destroy diagnostic decoder accepted a colliding context slot.'
}

$values['NativeContextDestroySlot02Stage'] = 0
$uncommittedRejected = $false
try {
    & "$PSScriptRoot/viogpu-native-context-destroy-diagnostics.ps1" `
        -ContextId 2 -DiagnosticData ([pscustomobject]$values) | Out-Null
} catch {
    $uncommittedRejected = $_.Exception.Message -like '*Stage value 0x00000000 is not recognized*'
}
if (-not $uncommittedRejected) {
    throw 'Native Context destroy diagnostic decoder accepted an uncommitted slot.'
}

Write-Host 'Native Context destroy diagnostic decoder fixture: PASS'
