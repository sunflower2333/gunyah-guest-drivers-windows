$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$devices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
$read = {
    param($object, $name)
    $property = $object.PSObject.Properties[$name]
    if ($null -eq $property) { return $null }
    return $property.Value
}
$rows = foreach ($device in $devices) {
    $key = 'HKLM:\SYSTEM\CurrentControlSet\Enum\' + $device.InstanceId
    $props = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
    [pscustomobject]@{
        Device = $device | Select-Object Status, ProblemCode, InstanceId, FriendlyName
        NativeStartStage = & $read $props 'NativeStartStage'
        NativeStartStatus = & $read $props 'NativeStartStatus'
        NativeStartDetail = & $read $props 'NativeStartDetail'
        NativePresentExecuteStage = & $read $props 'NativePresentExecuteStage'
        NativePresentExecuteStatus = & $read $props 'NativePresentExecuteStatus'
        NativePresentExecuteDetail = & $read $props 'NativePresentExecuteDetail'
        NativePresentExecuteFenceId = & $read $props 'NativePresentExecuteFenceId'
        NativePresentExecuteSourceResourceId = & $read $props 'NativePresentExecuteSourceResourceId'
        NativePresentExecuteDestinationResourceId = & $read $props 'NativePresentExecuteDestinationResourceId'
        NativePresentExecuteSourcePlacementState = & $read $props 'NativePresentExecuteSourcePlacementState'
        NativePresentExecuteDestinationPlacementState = & $read $props 'NativePresentExecuteDestinationPlacementState'
        NativePresentExecuteSourceResource2DState = & $read $props 'NativePresentExecuteSourceResource2DState'
        NativePresentExecuteDestinationResource2DState = & $read $props 'NativePresentExecuteDestinationResource2DState'
        NativePresentExecuteResetProvenanceValid = & $read $props 'NativePresentExecuteResetProvenanceValid'
        NativePresentExecuteHardwareResetState = & $read $props 'NativePresentExecuteHardwareResetState'
        NativePresentExecuteHardwareResetCallerRva = & $read $props 'NativePresentExecuteHardwareResetCallerRva'
    }
}

[pscustomobject]@{
    Machine = [Environment]::MachineName
    DriverPath = Join-Path $env:SystemRoot 'System32\drivers\viogpuwddm.sys'
    DriverHash = if (Test-Path -LiteralPath (Join-Path $env:SystemRoot 'System32\drivers\viogpuwddm.sys')) {
        (Get-FileHash -LiteralPath (Join-Path $env:SystemRoot 'System32\drivers\viogpuwddm.sys') -Algorithm SHA256).Hash.ToLowerInvariant()
    } else { $null }
    Devices = $rows
} | ConvertTo-Json -Depth 8
