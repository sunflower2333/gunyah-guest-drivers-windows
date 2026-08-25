[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-58081'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedVersion = '100.6.101.58081'
$expected = [ordered]@{
    'DroidVM_Test.cer' = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
    'drivers\viogpu\viogpuwddm.inf' = '7b47b552daef465cda89b3939e2b13f42034a2567ff4d8abec8b8786faa29df3'
    'drivers\viogpu\viogpuwddm.cat' = 'ead262aa8c8333d0c5d2fc88b22adabe583b045bdaeba6506971048868abb846'
    'drivers\viogpu\viogpuwddm.sys' = 'ea722e27cb01ac8fcde81a13bc20b3ac27a02d5e5ac48093b12c8d0500caba96'
    'drivers\viogpu\viogpud3d.dll' = 'c430f387409cb84743285e6788b50c2c18625dc255938e3360ce105cb180af06'
}

function Get-VioGpuDevice {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
    if ($devices.Count -ne 1) {
        throw "Expected one present virtio-gpu device, found $($devices.Count)."
    }
    $devices[0]
}

function Get-DriverSnapshot {
    $device = Get-VioGpuDevice
    $property = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
    if ([string]::IsNullOrWhiteSpace([string]$property.Data)) {
        throw "No driver key for $($device.InstanceId)."
    }
    $driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($property.Data)"
    $driverKey = Get-Item -LiteralPath $driverKeyPath
    $service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
    $imagePath = [Environment]::ExpandEnvironmentVariables([string]$service.GetValue('ImagePath', '')).Trim('"')
    if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
        $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
    }
    [pscustomobject]@{
        Status = [string]$device.Status
        ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) { $device.ProblemCode } else { $null }
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

$inf = Join-Path $PackageRoot 'drivers\viogpu\viogpuwddm.inf'
$infText = Get-Content -LiteralPath $inf -Raw
if ($infText -notmatch [regex]::Escape("DriverVer = 08/25/2026, $expectedVersion")) {
    throw "Unexpected viogpu package version."
}

$before = Get-DriverSnapshot
if ($before.Status -ne 'OK' -or ($null -ne $before.ProblemCode -and $before.ProblemCode -ne 0)) {
    throw "GPU device is unhealthy before install: $($before | ConvertTo-Json -Compress)"
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
    RebootRequired = $pnputilExitCode -eq 3010
} | ConvertTo-Json -Depth 7
