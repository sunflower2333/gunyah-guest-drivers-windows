[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-58063'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedVersion = '100.6.101.58063'
$expected = [ordered]@{
    'DroidVM_Test.cer' = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
    'viogpuwddm.inf' = '260def5c589d2c05c8221c3b83935a97dde9a42cbbaefe4e5505625871df9f48'
    'viogpuwddm.cat' = 'd085b588a40b42fc03b6b2f2d693625cefe5031439122f70b0be681ce91e28b9'
    'viogpuwddm.sys' = '09791bcf1b951c0984b9aa13bece29a863e8717862bc5d1eabbdf9a133e7f94c'
    'viogpud3d.dll' = '55e1653bb4dc2be1f273b0428ff99afabee06d77e3403430753ad55dd082383e'
}

function Get-VioGpuDevice {
    $devices = @(
        Get-PnpDevice -PresentOnly |
            Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' }
    )
    if ($devices.Count -ne 1) {
        throw "Expected exactly one present virtio-gpu device, found $($devices.Count)."
    }
    return $devices[0]
}

function Get-VioGpuDriverKey([string]$InstanceId) {
    $property = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Driver'
    if ($null -eq $property.PSObject.Properties['Data'] -or
        [string]::IsNullOrWhiteSpace([string]$property.Data)) {
        throw "Device '$InstanceId' has no driver registry key."
    }
    return "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($property.Data)"
}

function Resolve-DriverImagePath([string]$ImagePath) {
    $path = [Environment]::ExpandEnvironmentVariables($ImagePath.Trim('"'))
    if ($path.StartsWith('\??\', [StringComparison]::OrdinalIgnoreCase)) {
        $path = $path.Substring(4)
    }
    elseif ($path.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
        $path = Join-Path $env:SystemRoot $path.Substring(12)
    }
    elseif (-not [IO.Path]::IsPathRooted($path)) {
        $path = Join-Path $env:SystemRoot $path
    }
    return $path
}

function Get-VioGpuSnapshot {
    $device = Get-VioGpuDevice
    $driverKey = Get-VioGpuDriverKey $device.InstanceId
    $driver = Get-Item -LiteralPath $driverKey
    $service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
    $imagePath = Resolve-DriverImagePath ([string]$service.GetValue('ImagePath', ''))
    $imageHash = if (Test-Path -LiteralPath $imagePath -PathType Leaf) {
        (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    else {
        $null
    }
    $problemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) {
        $device.ProblemCode
    }
    else {
        $null
    }
    return [pscustomobject]@{
        Status = [string]$device.Status
        ProblemCode = $problemCode
        InstanceId = $device.InstanceId
        DriverKey = $driverKey
        DriverVersion = [string]$driver.GetValue('DriverVersion', '')
        InfPath = [string]$driver.GetValue('InfPath', '')
        Service = [string]$driver.GetValue('Service', '')
        ImagePath = $imagePath
        ImageHash = $imageHash
    }
}

foreach ($entry in $expected.GetEnumerator()) {
    $path = Join-Path $PackageRoot $entry.Key
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing package file: $path"
    }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $entry.Value) {
        throw "Hash mismatch for $($entry.Key): expected $($entry.Value), got $actual"
    }
}

$inf = Join-Path $PackageRoot 'viogpuwddm.inf'
$infText = Get-Content -LiteralPath $inf -Raw
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58063') {
    throw 'Unexpected viogpu package version.'
}

$before = Get-VioGpuSnapshot
$beforeKey = Get-Item -LiteralPath $before.DriverKey
foreach ($name in @($beforeKey.GetValueNames() | Where-Object { $_ -like 'NativeContextCreate*' })) {
    Remove-ItemProperty -LiteralPath $before.DriverKey -Name $name -Force
}

$cert = Join-Path $PackageRoot 'DroidVM_Test.cer'
Import-Certificate -FilePath $cert -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
Import-Certificate -FilePath $cert -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null

$pnputilOutput = @(& pnputil.exe /add-driver $inf /install 2>&1)
$pnputilExitCode = $LASTEXITCODE
if ($pnputilExitCode -notin @(0, 3010)) {
    throw "pnputil failed ($pnputilExitCode): $($pnputilOutput -join "`n")"
}

Start-Sleep -Seconds 5
$after = Get-VioGpuSnapshot
if ($after.DriverVersion -ne $expectedVersion) {
    throw "Driver did not activate version $expectedVersion; active version is $($after.DriverVersion)."
}
if ($after.Status -ne 'OK' -or ($null -ne $after.ProblemCode -and $after.ProblemCode -ne 0)) {
    throw "Driver is not healthy after install: status=$($after.Status), code=$($after.ProblemCode)."
}
if ($after.ImageHash -ne $expected['viogpuwddm.sys']) {
    throw "Active DriverStore SYS hash mismatch: expected $($expected['viogpuwddm.sys']), got $($after.ImageHash)."
}

[pscustomobject]@{
    PackageVersion = $expectedVersion
    PnpUtilExitCode = $pnputilExitCode
    PnpUtilOutput = $pnputilOutput
    Before = $before
    After = $after
} | ConvertTo-Json -Depth 7
