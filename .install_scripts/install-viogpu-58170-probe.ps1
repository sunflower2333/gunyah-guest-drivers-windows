[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\Administrator\Desktop\ci-33192485200\package',
    [string]$OutputDirectory = 'C:\Users\Administrator\viogpu-58170\kmt'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedVersion = '100.6.101.58170'
$inf = Join-Path $PackageRoot 'drivers\viogpu\viogpuwddm.inf'
$cert = Join-Path $PackageRoot 'DroidVM_Test.cer'
foreach ($path in @($inf, $cert)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing package file: $path"
    }
}

function Get-GpuDevice {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
    })
    if ($devices.Count -ne 1) {
        throw "Expected one present virtio-gpu device, found $($devices.Count)."
    }
    $devices[0]
}

function Get-GpuSnapshot {
    $device = Get-GpuDevice
    $property = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
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
        InstanceId = [string]$device.InstanceId
        DriverKeyPath = $driverKeyPath
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        ImagePath = $imagePath
        ImageHash = if (Test-Path -LiteralPath $imagePath -PathType Leaf) {
            (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()
        } else { $null }
    }
}

$before = Get-GpuSnapshot
$driverKey = Get-Item -LiteralPath $before.DriverKeyPath
$stale = @($driverKey.GetValueNames() | Where-Object {
    $_.StartsWith('NativeContext', [StringComparison]::Ordinal)
})
foreach ($name in $stale) {
    Remove-ItemProperty -LiteralPath $before.DriverKeyPath -Name $name -ErrorAction Stop
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Get-ChildItem -LiteralPath $OutputDirectory -Force -ErrorAction SilentlyContinue |
    Remove-Item -Force -Recurse

Import-Certificate -FilePath $cert -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
Import-Certificate -FilePath $cert -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null
$installOutput = @(& pnputil.exe /add-driver $inf /install 2>&1)
$installExitCode = $LASTEXITCODE
if ($installExitCode -notin @(0, 3010)) {
    throw "pnputil failed ($installExitCode): $($installOutput -join "`n")"
}

Start-Sleep -Seconds 8
$after = Get-GpuSnapshot
if ($after.DriverVersion -ne $expectedVersion) {
    throw "Active version is '$($after.DriverVersion)', expected '$expectedVersion'."
}
if ($after.Status -ne 'OK' -or ($null -ne $after.ProblemCode -and $after.ProblemCode -ne 0)) {
    throw "GPU device is unhealthy after install: $($after | ConvertTo-Json -Compress)"
}

[pscustomobject]@{
    PackageVersion = $expectedVersion
    PackageRoot = $PackageRoot
    PnpUtilExitCode = $installExitCode
    PnpUtilOutput = $installOutput
    StaleDiagnosticsRemoved = $stale
    Before = $before
    After = $after
    RebootRequired = $installExitCode -eq 3010
} | ConvertTo-Json -Depth 8
