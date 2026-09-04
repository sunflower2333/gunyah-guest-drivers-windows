[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-build-33167214557',
    [switch]$StageOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedVersion = '100.6.101.58169'
$expected = [ordered]@{
    'DroidVM_Test.cer' = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
    'drivers\viogpu\viogpuwddm.inf' = '61bce95b3e55fcb965b32f2f2f89869ad5c4a9e0fbbfdf2393d7b3d692df82a0'
    'drivers\viogpu\viogpuwddm.cat' = '17f32e142f10c09131a39cd3fd6756e48850f6bd730c1f3e75ebb52b6f44d4fc'
    'drivers\viogpu\viogpuwddm.sys' = 'a31fab429ac858138ea308347606eb05ed4571015fe4175b71b9209fe1e6cb58'
    'drivers\viogpu\viogpud3d.dll' = '4e4040f074a58efc0716bdb228d6d78fa4bbac8a28cbc94b9850012fc2f04470'
    'drivers\viogpu\viogpuwddm.pdb' = '5d53a742f62668677af03151acda87cddcddf51d114d1787efb13f377843a285'
    'drivers\viogpu\viogpuwddm.map' = 'f0b9ea321f279a30c0c4007d46535a03a53dbc55265925df80a71d9bc01ebec2'
}

function Get-GpuSnapshot {
    $devices = @(
        Get-PnpDevice -PresentOnly |
            Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' }
    )
    if ($devices.Count -ne 1) {
        throw "Expected one present virtio-gpu device, found $($devices.Count)."
    }

    $device = $devices[0]
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
    elseif (-not [IO.Path]::IsPathRooted($imagePath)) {
        $imagePath = Join-Path $env:SystemRoot $imagePath
    }

    [pscustomobject]@{
        Status = [string]$device.Status
        ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) { $device.ProblemCode } else { $null }
        InstanceId = [string]$device.InstanceId
        DriverKey = $driverKeyPath
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
        throw "Hash mismatch for $($entry.Key): expected $($entry.Value), got $actual"
    }
}

$inf = Join-Path $PackageRoot 'drivers\viogpu\viogpuwddm.inf'
$infText = Get-Content -LiteralPath $inf -Raw
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58169') {
    throw 'Unexpected viogpu package version.'
}

$before = Get-GpuSnapshot
if ($before.Status -ne 'OK' -or ($null -ne $before.ProblemCode -and $before.ProblemCode -ne 0)) {
    throw "GPU device is unhealthy before staging: $($before | ConvertTo-Json -Compress)"
}

$cert = Join-Path $PackageRoot 'DroidVM_Test.cer'
Import-Certificate -FilePath $cert -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
Import-Certificate -FilePath $cert -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null

$arguments = @('/add-driver', $inf)
if (-not $StageOnly) {
    $arguments += '/install'
}
$pnputilOutput = @(& pnputil.exe @arguments 2>&1)
$pnputilExitCode = $LASTEXITCODE
if ($pnputilExitCode -notin @(0, 3010)) {
    throw "pnputil failed ($pnputilExitCode): $($pnputilOutput -join "`n")"
}

if ($StageOnly) {
    [pscustomobject]@{
        PackageVersion = $expectedVersion
        StageOnly = $true
        PnpUtilExitCode = $pnputilExitCode
        PnpUtilOutput = $pnputilOutput
        Before = $before
    } | ConvertTo-Json -Depth 7
    exit 0
}

Start-Sleep -Seconds 8
$after = Get-GpuSnapshot
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
    StageOnly = $false
    PnpUtilExitCode = $pnputilExitCode
    PnpUtilOutput = $pnputilOutput
    Before = $before
    After = $after
    RebootRequired = $pnputilExitCode -eq 3010
} | ConvertTo-Json -Depth 7
