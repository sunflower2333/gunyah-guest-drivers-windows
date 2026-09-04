[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-build-33135644735'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedVersion = '100.6.101.58165'
$expected = [ordered]@{
    'DroidVM_Test.cer' = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
    'drivers\viogpu\viogpuwddm.inf' = '076a5943ad43db3ecfd3ca200d42fb12d78b14ac99d3ceaf360f8be6ba5a80d3'
    'drivers\viogpu\viogpuwddm.cat' = 'eafb840ff95c0e1cd0e1ebb53d4757fca3d5a84510531ab39660d53fc1bb4539'
    'drivers\viogpu\viogpuwddm.sys' = 'e235757e6ad0579c46e01ae8e0f6bf649f119aef6a3d2b07519f08623fcaa4d8'
    'drivers\viogpu\viogpud3d.dll' = '239a19134333953da96a78bd3f79ac6c1a0cae529ca24a8b890f562c727333fc'
}

$parameterNames = @(
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

function Get-GpuSnapshot {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
    if ($devices.Count -ne 1) {
        throw "Expected one present virtio-gpu device, found $($devices.Count)."
    }
    $device = $devices[0]
    $property = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
    if ([string]::IsNullOrWhiteSpace([string]$property.Data)) {
        throw "No driver key for $($device.InstanceId)."
    }
    $driverKey = Get-Item -LiteralPath ("Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\{0}" -f $property.Data)
    $service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
    $imagePath = [Environment]::ExpandEnvironmentVariables([string]$service.GetValue('ImagePath', '')).Trim('"')
    if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
        $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
    }
    [pscustomobject]@{
        Status = [string]$device.Status
        ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) { $device.ProblemCode } else { $null }
        InstanceId = [string]$device.InstanceId
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        InfPath = [string]$driverKey.GetValue('InfPath', '')
        ImagePath = $imagePath
        ImageHash = if (Test-Path -LiteralPath $imagePath -PathType Leaf) {
            (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()
        } else { $null }
    }
}

foreach ($entry in $expected.GetEnumerator()) {
    $path = Join-Path $PackageRoot $entry.Key
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing package file: $path"
    }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $entry.Value) {
        throw "Hash mismatch for $($entry.Key): $actual"
    }
}

$before = Get-GpuSnapshot
if ($before.Status -ne 'OK' -or ($null -ne $before.ProblemCode -and $before.ProblemCode -ne 0)) {
    throw "GPU device is unhealthy before install: $($before | ConvertTo-Json -Compress)"
}

$cert = Join-Path $PackageRoot 'DroidVM_Test.cer'
Import-Certificate -FilePath $cert -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
Import-Certificate -FilePath $cert -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null

$inf = Join-Path $PackageRoot 'drivers\viogpu\viogpuwddm.inf'
$pnputilOutput = @(& pnputil.exe /add-driver $inf /install 2>&1)
$pnputilExitCode = $LASTEXITCODE
if ($pnputilExitCode -notin @(0, 3010)) {
    throw "pnputil failed ($pnputilExitCode): $($pnputilOutput -join "`n")"
}

Start-Sleep -Seconds 8
$after = Get-GpuSnapshot
if ($after.DriverVersion -ne $expectedVersion) {
    throw "Active version is $($after.DriverVersion), expected $expectedVersion."
}
if ($after.Status -ne 'OK' -or ($null -ne $after.ProblemCode -and $after.ProblemCode -ne 0)) {
    throw "GPU device is unhealthy after install: $($after | ConvertTo-Json -Compress)"
}

$driverKey = Get-Item -LiteralPath ("Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\{0}" -f (
        (Get-PnpDeviceProperty -InstanceId $after.InstanceId -KeyName 'DEVPKEY_Device_Driver').Data
    ))
$parameterSnapshot = [ordered]@{}
foreach ($name in $parameterNames) {
    $parameterSnapshot[$name] = if ($null -ne $driverKey.GetValueNames() -and $driverKey.GetValueNames() -contains $name) {
        $driverKey.GetValue($name)
    } else { $null }
}

[pscustomobject]@{
    PackageVersion = $expectedVersion
    PnpUtilExitCode = $pnputilExitCode
    PnpUtilOutput = $pnputilOutput
    Before = $before
    After = $after
    NativeContextGetParam = $parameterSnapshot
    RebootRequired = $pnputilExitCode -eq 3010
} | ConvertTo-Json -Depth 8
