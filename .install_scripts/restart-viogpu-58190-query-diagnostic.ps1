[CmdletBinding()]
param(
    [int]$WaitSeconds = 10
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedVersion = '100.6.101.58190'
$expectedInf = 'oem26.inf'
$expectedSysHash = '8ab40857646be6f0e40a0aec7d63bc80190f51af1d7afc5707aeae9b5bbacdf5'
$expectedUmdHash = '4aef90a8a909f19d7ab10e4874515575cfa20b8ef757c863c8717e814fa86520'
$expectedSigner = 'CN=DroidVM Test'

$devices = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
})
if ($devices.Count -ne 1) {
    throw "Expected one present virtio-gpu device, found $($devices.Count)."
}

$device = $devices[0]
$problem = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode'
$driverProperty = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
$driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($driverProperty.Data)"
$driverKey = Get-Item -LiteralPath $driverKeyPath
$service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
$imagePath = [Environment]::ExpandEnvironmentVariables([string]$service.GetValue('ImagePath', '')).Trim('"')
if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
    $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
}
$image = Get-Item -LiteralPath $imagePath
$umdPath = Join-Path $image.DirectoryName 'viogpud3d.dll'
$signature = Get-AuthenticodeSignature -LiteralPath $image.FullName

if ($device.Status -ne 'Error' -or
    [int]$problem.Data -ne 43 -or
    [string]$driverKey.GetValue('DriverVersion', '') -ne $expectedVersion -or
    [string]$driverKey.GetValue('InfPath', '') -ne $expectedInf -or
    [int]$driverKey.GetValue('RenderOnly', -1) -ne 1 -or
    (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedSysHash -or
    (Get-FileHash -LiteralPath $umdPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedUmdHash -or
    $signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
    $null -eq $signature.SignerCertificate -or
    $signature.SignerCertificate.Subject -ne $expectedSigner) {
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
