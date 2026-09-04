[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-32797743728'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedVersion = '100.6.101.58064'
$expectedSysHash = 'bfcc37c0963b42bd286d02795818ed6731e53c97dbbe9a0e136c63986bab85b1'
$expected = [ordered]@{
    'DroidVM_Test.cer' = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
    'viogpuwddm.inf' = '5f82ed0fe3ec851652172ba7a14e1e7a4d76bec1de65135720afa4db90487219'
    'viogpuwddm.cat' = '06ad22cf777875530d7c39b13ac674fcd5989860a61d4c513f79e68054adad6b'
    'viogpuwddm.sys' = $expectedSysHash
    'viogpud3d.dll' = '32b8cee2d9004409a08e44d661253591631b94008ed82ed5dc45e94443e2720d'
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
$beforeKey = Get-Item -LiteralPath $before.DriverKey
foreach ($name in @($beforeKey.GetValueNames() | Where-Object { $_ -like 'NativeContextCreate*' })) {
    Remove-ItemProperty -LiteralPath $before.DriverKey -Name $name -Force
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
if ($after.Status -ne 'OK' -or ($null -ne $after.ProblemCode -and $after.ProblemCode -ne 0)) { throw "GPU device unhealthy: $($after | ConvertTo-Json -Compress)" }
if ($after.ImageHash -ne $expectedSysHash) { throw "Active SYS hash is $($after.ImageHash), expected $expectedSysHash." }

[pscustomobject]@{
    PackageVersion = $expectedVersion
    PnpUtilExitCode = $pnputilExitCode
    PnpUtilOutput = $pnputilOutput
    Before = $before
    After = $after
} | ConvertTo-Json -Depth 7
