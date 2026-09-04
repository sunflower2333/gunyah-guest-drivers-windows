[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-58075'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedVersion = '100.6.101.58075'
$expected = [ordered]@{
    'DroidVM_Test.cer' = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
    'drivers\viogpu\viogpuwddm.inf' = '81193df3f732d6d67faf162edfbb1fb13ef621c3cdb5cdbab17fd39e4def7093'
    'drivers\viogpu\viogpuwddm.cat' = 'd7d328b9b1e237c9ad788042a5e0f133951e164c44e67f1564f7461ed35c289c'
    'drivers\viogpu\viogpuwddm.sys' = 'b4005ff7b788ebff26cbfa24e02e1957c13b94318fdf67ff0d21ed3468191a0b'
    'drivers\viogpu\viogpud3d.dll' = '3d9854705c99a2c2661e3b5e1e0e0698d05c071999fd503f331f2a1c23b1235c'
}

function Get-VioGpuDevice {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
    if ($devices.Count -ne 1) {
        throw "Expected one present virtio-gpu device, found $($devices.Count)."
    }
    $devices[0]
}

function Get-DriverKey([string]$InstanceId) {
    $property = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Driver'
    if ([string]::IsNullOrWhiteSpace([string]$property.Data)) {
        throw "No driver key for $InstanceId."
    }
    "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($property.Data)"
}

function Resolve-ImagePath([string]$ImagePath) {
    $path = [Environment]::ExpandEnvironmentVariables($ImagePath.Trim('"'))
    if ($path.StartsWith('\??\', [StringComparison]::OrdinalIgnoreCase)) {
        return $path.Substring(4)
    }
    if ($path.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
        return Join-Path $env:SystemRoot $path.Substring(12)
    }
    if (-not [IO.Path]::IsPathRooted($path)) {
        return Join-Path $env:SystemRoot $path
    }
    $path
}

function Get-DriverSnapshot {
    $device = Get-VioGpuDevice
    $driverKeyPath = Get-DriverKey $device.InstanceId
    $driverKey = Get-Item -LiteralPath $driverKeyPath
    $service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
    $imagePath = Resolve-ImagePath ([string]$service.GetValue('ImagePath', ''))
    [pscustomobject]@{
        Status = [string]$device.Status
        ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) { $device.ProblemCode } else { $null }
        InstanceId = $device.InstanceId
        DriverKey = $driverKeyPath
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        InfPath = [string]$driverKey.GetValue('InfPath', '')
        ImagePath = $imagePath
        ImageHash = if (Test-Path -LiteralPath $imagePath -PathType Leaf) {
            (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        else {
            $null
        }
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

$before = Get-DriverSnapshot
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

Start-Sleep -Seconds 5
$after = Get-DriverSnapshot
if ($after.DriverVersion -ne $expectedVersion) {
    throw "Active version is $($after.DriverVersion), expected $expectedVersion."
}
if ($after.Status -ne 'OK' -or ($null -ne $after.ProblemCode -and $after.ProblemCode -ne 0)) {
    throw "GPU device is unhealthy after install: $($after | ConvertTo-Json -Compress)"
}
if ($after.ImageHash -ne $expected['drivers\viogpu\viogpuwddm.sys']) {
    throw "Active SYS hash is $($after.ImageHash), expected $($expected['drivers\viogpu\viogpuwddm.sys'])."
}

[pscustomobject]@{
    PackageVersion = $expectedVersion
    PnpUtilExitCode = $pnputilExitCode
    PnpUtilOutput = $pnputilOutput
    Before = $before
    After = $after
} | ConvertTo-Json -Depth 7
