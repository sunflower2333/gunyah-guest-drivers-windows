[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-58070',
    [string]$ExpectedVersion = '100.6.101.58070',
    [string]$ExpectedHash = 'cd70f0f89a1c1eb897faf284ae5b4dc7a0b5fc4341236a2731ffff8ea5ddcb5c'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedVersion = $ExpectedVersion
$expectedHash = $ExpectedHash
$inf = Join-Path $PackageRoot 'drivers\viogpu\viogpuwddm.inf'
$cert = Join-Path $PackageRoot 'DroidVM_Test.cer'

foreach ($path in @($inf, $cert)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing package file: $path"
    }
}

Import-Certificate -FilePath $cert -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
Import-Certificate -FilePath $cert -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null
$output = @(& pnputil.exe /add-driver $inf /install 2>&1)
$exitCode = $LASTEXITCODE
if ($exitCode -notin @(0, 3010)) {
    throw "pnputil failed ($exitCode): $($output -join "`n")"
}

Start-Sleep -Seconds 5
$device = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
if ($device.Count -ne 1) {
    throw "Expected exactly one virtio-gpu device, found $($device.Count)."
}
$property = Get-PnpDeviceProperty -InstanceId $device[0].InstanceId -KeyName 'DEVPKEY_Device_Driver'
$driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($property.Data)"
$driverKey = Get-Item -LiteralPath $driverKeyPath
$service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
$imagePath = [Environment]::ExpandEnvironmentVariables(([string]$service.GetValue('ImagePath', '')).Trim('"'))
if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
    $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
}

$snapshot = [pscustomobject]@{
    PnpUtilExitCode = $exitCode
    PnpUtilOutput = $output
    Status = [string]$device[0].Status
    DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
    ImagePath = $imagePath
    ImageHash = (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()
}
if ($snapshot.DriverVersion -ne $expectedVersion -or $snapshot.Status -ne 'OK' -or $snapshot.ImageHash -ne $expectedHash) {
    throw "Unexpected active driver state: $($snapshot | ConvertTo-Json -Compress)"
}
$snapshot | ConvertTo-Json -Depth 6
