[CmdletBinding()]
param(
    [int]$WaitSeconds = 10
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedVersion = '100.6.101.58192'
$expectedInf = 'oem27.inf'
$expectedSysHash = '09afe04f9a1b83de5a40c5efa6324e664d466238219c8a2b0f5c60531be98b4d'
$expectedUmdHash = 'efcb7b56431be589193ee67c509b182ebf7ba537d956344d873451e33936a475'
$expectedSigner = 'CN=DroidVM Test'

$devices = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
})
if ($devices.Count -ne 1) {
    throw "Expected one present virtio-gpu device, found $($devices.Count)."
}

$device = $devices[0]
$problem = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode'
$problemStatus = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemStatus'
$driverProperty = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
$driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($driverProperty.Data)"
$driverKey = Get-Item -LiteralPath $driverKeyPath
$service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
$serviceParameters = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm\Parameters'
$serviceState = Get-CimInstance Win32_SystemDriver -Filter "Name='VioGpuWddm'"
$imagePath = [Environment]::ExpandEnvironmentVariables([string]$service.GetValue('ImagePath', '')).Trim('"')
if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
    $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
}
$image = Get-Item -LiteralPath $imagePath
$umdPath = Join-Path $image.DirectoryName 'viogpud3d.dll'
$signature = Get-AuthenticodeSignature -LiteralPath $image.FullName

if ($device.Status -ne 'Error' -or
    [int]$problem.Data -ne 43 -or
    [int64]$problemStatus.Data -ne 0 -or
    [string]$driverKey.GetValue('DriverVersion', '') -ne $expectedVersion -or
    [string]$driverKey.GetValue('InfPath', '') -ne $expectedInf -or
    [int]$driverKey.GetValue('RenderOnly', -1) -ne 1 -or
    [int]$serviceParameters.GetValue('RenderOnly', -1) -ne 1 -or
    (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedSysHash -or
    (Get-FileHash -LiteralPath $umdPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedUmdHash -or
    $signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
    $null -eq $signature.SignerCertificate -or
    $signature.SignerCertificate.Subject -ne $expectedSigner -or
    $serviceState.State -ne 'Stopped') {
    throw 'Refusing diagnostic restart from an unexpected driver or device state.'
}

$diagnosticNames = @($driverKey.GetValueNames() | Where-Object {
    $_ -like 'NativeStart*' -or $_ -like 'NativeQueryAdapterInfo*'
})
foreach ($name in $diagnosticNames) {
    Remove-ItemProperty -LiteralPath $driverKeyPath -Name $name
}

$restartStartedAt = Get-Date
$restartOutput = @(& pnputil.exe /restart-device $device.InstanceId 2>&1)
$restartExitCode = $LASTEXITCODE
Start-Sleep -Seconds $WaitSeconds

$after = Get-PnpDevice -PresentOnly -InstanceId $device.InstanceId
$afterProblem = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode'
$afterService = Get-CimInstance Win32_SystemDriver -Filter "Name='VioGpuWddm'"
$afterKey = Get-Item -LiteralPath $driverKeyPath
$diagnostics = [ordered]@{}
foreach ($name in $afterKey.GetValueNames() | Where-Object {
    $_ -like 'NativeStart*' -or $_ -like 'NativeQueryAdapterInfo*'
} | Sort-Object) {
    $diagnostics[$name] = $afterKey.GetValue(
        $name,
        $null,
        [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames
    )
}

[ordered]@{
    RestartStartedAt = $restartStartedAt.ToString('o')
    RestartExitCode = $restartExitCode
    RestartOutput = $restartOutput
    RemovedDiagnostics = $diagnosticNames
    Status = [string]$after.Status
    ProblemCode = $afterProblem.Data
    ServiceState = [string]$afterService.State
    Diagnostics = $diagnostics
} | ConvertTo-Json -Depth 6
