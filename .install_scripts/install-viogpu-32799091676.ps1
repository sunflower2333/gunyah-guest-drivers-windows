[CmdletBinding()]
param([string]$PackageRoot = 'C:\Users\Administrator\viogpu-32799091676')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$expectedVersion = '100.6.101.58065'
$expectedSysHash = '6210e6955128ef378ba91c4b9acfe048bfdd6d54b936760d09f824fc48cadbd6'
$expected = @{
    'DroidVM_Test.cer' = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
    'viogpuwddm.inf' = 'c106a99e6250347d182c9b28e5eecaa86c8df459941f2d03b28e70c707a1eff3'
    'viogpuwddm.cat' = '58dbd85a5427ed4acc9cc1338bbf63e78b6de6e0749e9cbb3f89ea1bd4b9842f'
    'viogpuwddm.sys' = $expectedSysHash
    'viogpud3d.dll' = '0d38d7cccb2290b016ecff4e736252b2f28425f24c2e6e151b583ecfc35e4153'
}

function Get-GpuDevice {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
    if ($devices.Count -ne 1) { throw "Expected one present virtio-gpu device, found $($devices.Count)." }
    $devices[0]
}
function Get-DriverKey([string]$InstanceId) {
    $property = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Driver'
    if ([string]::IsNullOrWhiteSpace([string]$property.Data)) { throw "Missing driver key for $InstanceId." }
    "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($property.Data)"
}
function Get-Snapshot {
    $device = Get-GpuDevice
    $keyPath = Get-DriverKey $device.InstanceId
    $key = Get-Item -LiteralPath $keyPath
    $service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
    $imagePath = [Environment]::ExpandEnvironmentVariables(([string]$service.GetValue('ImagePath', '')).Trim('"'))
    if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
        $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
    }
    [pscustomobject]@{
        Status = [string]$device.Status
        InstanceId = $device.InstanceId
        DriverVersion = [string]$key.GetValue('DriverVersion', '')
        InfPath = [string]$key.GetValue('InfPath', '')
        ImagePath = $imagePath
        ImageHash = if (Test-Path -LiteralPath $imagePath -PathType Leaf) { (Get-FileHash $imagePath -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
    }
}

foreach ($entry in $expected.GetEnumerator()) {
    $path = if ($entry.Key -eq 'DroidVM_Test.cer') { Join-Path $PackageRoot $entry.Key } else { Join-Path $PackageRoot "drivers\viogpu\$($entry.Key)" }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing package file: $path" }
    $actual = (Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $entry.Value) { throw "Hash mismatch for $($entry.Key): $actual" }
}

$before = Get-Snapshot
$keyPath = Get-DriverKey $before.InstanceId
$key = Get-Item -LiteralPath $keyPath
foreach ($name in @($key.GetValueNames() | Where-Object { $_ -like 'NativeContextCreate*' })) {
    Remove-ItemProperty -LiteralPath $keyPath -Name $name -Force
}

Import-Certificate -FilePath (Join-Path $PackageRoot 'DroidVM_Test.cer') -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
Import-Certificate -FilePath (Join-Path $PackageRoot 'DroidVM_Test.cer') -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null
$pnputilOutput = @(& pnputil.exe /add-driver (Join-Path $PackageRoot 'drivers\viogpu\viogpuwddm.inf') /install 2>&1)
$pnputilExitCode = $LASTEXITCODE
if ($pnputilExitCode -notin @(0, 3010)) { throw "pnputil failed ($pnputilExitCode): $($pnputilOutput -join "`n")" }
Start-Sleep -Seconds 5
$after = Get-Snapshot
if ($after.DriverVersion -ne $expectedVersion) { throw "Active version is $($after.DriverVersion), expected $expectedVersion." }
if ($after.Status -ne 'OK') { throw "GPU device is not OK: $($after | ConvertTo-Json -Compress)" }
if ($after.ImageHash -ne $expectedSysHash) { throw "Active SYS hash is $($after.ImageHash), expected $expectedSysHash." }
[pscustomobject]@{ PackageVersion = $expectedVersion; PnpUtilExitCode = $pnputilExitCode; Before = $before; After = $after; PnpUtilOutput = $pnputilOutput } | ConvertTo-Json -Depth 7
