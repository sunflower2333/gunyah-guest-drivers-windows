[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-58068'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedVersion = '100.6.101.58068'
$expected = [ordered]@{
    'DroidVM_Test.cer' = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
    'viogpuwddm.inf' = '9125736453d114e81088aec7505657dd9ed398e11eeb4fdf79053f43c2e1be30'
    'viogpuwddm.cat' = '4cf9623d9b4825c87622c2ac6d26726007686e22ae6e4b6104d34a5dfef2da20'
    'viogpuwddm.sys' = '8e3f2496967b2078c4830e3705a077113fa2ef7b794e7b1768de768f5d96921e'
    'viogpud3d.dll' = '40d7c90de53dc68ec3b0adbd20bd6ed8ac2381fa9d725bc78cdb4e633919bf2f'
}

function Get-VioGpuDevice {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
    if ($devices.Count -ne 1) { throw "Expected one present virtio-gpu device, found $($devices.Count)." }
    $devices[0]
}

function Get-DriverKey([string]$InstanceId) {
    $property = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Driver'
    if ([string]::IsNullOrWhiteSpace([string]$property.Data)) { throw "No driver key for $InstanceId." }
    "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($property.Data)"
}

function Resolve-ImagePath([string]$ImagePath) {
    $path = [Environment]::ExpandEnvironmentVariables($ImagePath.Trim('"'))
    if ($path.StartsWith('\??\', [StringComparison]::OrdinalIgnoreCase)) { return $path.Substring(4) }
    if ($path.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) { return Join-Path $env:SystemRoot $path.Substring(12) }
    if (-not [IO.Path]::IsPathRooted($path)) { return Join-Path $env:SystemRoot $path }
    $path
}

function Get-Snapshot {
    $device = Get-VioGpuDevice
    $driverKey = Get-DriverKey $device.InstanceId
    $driver = Get-Item -LiteralPath $driverKey
    $service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
    $imagePath = Resolve-ImagePath ([string]$service.GetValue('ImagePath', ''))
    [pscustomobject]@{
        Status = [string]$device.Status
        ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) { $device.ProblemCode } else { $null }
        InstanceId = $device.InstanceId
        DriverKey = $driverKey
        DriverVersion = [string]$driver.GetValue('DriverVersion', '')
        InfPath = [string]$driver.GetValue('InfPath', '')
        ImagePath = $imagePath
        ImageHash = if (Test-Path -LiteralPath $imagePath -PathType Leaf) { (Get-FileHash $imagePath -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
    }
}

foreach ($entry in $expected.GetEnumerator()) {
    $path = Join-Path $PackageRoot $entry.Key
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing package file: $path" }
    $actual = (Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $entry.Value) { throw "Hash mismatch for $($entry.Key): $actual" }
}

$before = Get-Snapshot
if ($before.Status -ne 'OK' -or ($null -ne $before.ProblemCode -and $before.ProblemCode -ne 0)) {
    throw "GPU device is unhealthy before install: $($before | ConvertTo-Json -Compress)"
}

$cert = Join-Path $PackageRoot 'DroidVM_Test.cer'
Import-Certificate -FilePath $cert -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
Import-Certificate -FilePath $cert -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null

$inf = Join-Path $PackageRoot 'viogpuwddm.inf'
$pnputilOutput = @(& pnputil.exe /add-driver $inf /install 2>&1)
$pnputilExitCode = $LASTEXITCODE
if ($pnputilExitCode -notin @(0, 3010)) { throw "pnputil failed ($pnputilExitCode): $($pnputilOutput -join "`n")" }

Start-Sleep -Seconds 5
$after = Get-Snapshot
if ($after.DriverVersion -ne $expectedVersion) { throw "Active version is $($after.DriverVersion), expected $expectedVersion." }
if ($after.Status -ne 'OK' -or ($null -ne $after.ProblemCode -and $after.ProblemCode -ne 0)) {
    throw "GPU device unhealthy after install: $($after | ConvertTo-Json -Compress)"
}
if ($after.ImageHash -ne $expected['viogpuwddm.sys']) {
    throw "Active SYS hash is $($after.ImageHash), expected $($expected['viogpuwddm.sys'])."
}

[pscustomobject]@{
    PackageVersion = $expectedVersion
    PnpUtilExitCode = $pnputilExitCode
    PnpUtilOutput = $pnputilOutput
    Before = $before
    After = $after
} | ConvertTo-Json -Depth 7
