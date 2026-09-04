[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\DroidVM\viogpu-58030\viogpuwddm',
    [string]$CertificatePath = 'C:\DroidVM\viogpu-58030\DroidVM_Test.cer',
    [string]$OutputDirectory = 'C:\DroidVM\viogpu-58030\runtime-logs',
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
trap {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'The viogpu package must be installed from an elevated PowerShell session.'
}

$expectedHashes = [ordered]@{
    'viogpuwddm.sys' = '03fe6d8e2ea9afa9310f7aca20b4394fb376a53ea87d6d0d7c15b5e92181cc27'
    'viogpud3d.dll' = '6ca505d73fab88950e493be8205923dbb3043d0b657017fb17321681f90a1184'
    'viogpuwddm.cat' = 'b318e80365b64ebd1289b18a3fc311fc31698bbd20230a57204c09c7aaa0e6c0'
    'viogpuwddm.inf' = '1fa0cde4af1b211f7be8f7bf8240e3bc5384c31d201003a3940f0673de61dca8'
    'viogpuwddm.pdb' = 'a5362e408dc2b3a9e9b025dae1e78781da41ed8f894445fabd4c3c614c1421a4'
    'viogpuwddm.map' = '58d4323638fd08fee96263bf2ab092738238b9b6c6b8d1b79026c5b442756bff'
    'viogpud3d.pdb' = '610a835c10b80aae97ad4f6cc181361a2c564231ffbe24be7754b18fbab4fa03'
}
$expectedCertificateHash = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'

if (-not (Test-Path -LiteralPath $PackageRoot -PathType Container)) {
    throw "Missing viogpu package directory: $PackageRoot"
}
if (-not (Test-Path -LiteralPath $CertificatePath -PathType Leaf)) {
    throw "Missing test certificate: $CertificatePath"
}

$certificateHash = (Get-FileHash -LiteralPath $CertificatePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($certificateHash -ne $expectedCertificateHash) {
    throw "SHA-256 mismatch for '$CertificatePath': expected $expectedCertificateHash, got $certificateHash"
}

foreach ($entry in $expectedHashes.GetEnumerator()) {
    $path = Join-Path $PackageRoot $entry.Key
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing package file: $path"
    }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $entry.Value) {
        throw "SHA-256 mismatch for '$path': expected $($entry.Value), got $actual"
    }
}

$unexpectedFiles = @(
    Get-ChildItem -LiteralPath $PackageRoot -File |
        Where-Object { -not $expectedHashes.Contains($_.Name) }
)
if ($unexpectedFiles.Count -ne 0) {
    throw "The viogpu package directory contains unexpected files: $($unexpectedFiles.Name -join ', ')"
}

$infPath = Join-Path $PackageRoot 'viogpuwddm.inf'
$infText = Get-Content -LiteralPath $infPath -Raw
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58030') {
    throw "The staged INF is not version 100.6.101.58030: $infPath"
}
if ($infText -notmatch 'PCI\\VEN_1AF4&DEV_1050' -or $infText -match '(?i)netkvm|rdmapool|droidvmpool|DRVM0001') {
    throw 'The staged INF does not describe only the expected unprotected virtio-gpu package.'
}

$devices = @(
    Get-PnpDevice -PresentOnly |
        Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' }
)
if ($devices.Count -ne 1) {
    throw "Expected one present virtio-gpu device, found $($devices.Count)."
}

if ($ValidateOnly) {
    [pscustomobject]@{
        ValidatedAt = (Get-Date).ToString('o')
        PackageVersion = '100.6.101.58030'
        PackageRoot = $PackageRoot
        CertificatePath = $CertificatePath
        FileCount = $expectedHashes.Count
        InstanceId = $devices[0].InstanceId
        DeviceStatus = $devices[0].Status
    } | ConvertTo-Json -Depth 3
    return
}

$removedLogs = @()
$logExtensions = @('.etl', '.json', '.csv', '.xml', '.txt')
$logDirectories = @(
    58016..58029 | ForEach-Object { "C:\DroidVM\viogpu-$_\runtime-logs" }
) + @($OutputDirectory)
foreach ($directory in $logDirectories) {
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        continue
    }
    $logs = @(
        Get-ChildItem -LiteralPath $directory -File |
            Where-Object { $_.Extension -in $logExtensions }
    )
    $removedLogs += $logs | Select-Object FullName, Length, LastWriteTimeUtc
    $logs | Remove-Item -Force
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$instanceId = $devices[0].InstanceId
$rootCertificate = Import-Certificate -FilePath $CertificatePath -CertStoreLocation 'Cert:\LocalMachine\Root'
$publisherCertificate = Import-Certificate -FilePath $CertificatePath -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher'
$installStartedAt = Get-Date
$installOutput = @(& pnputil.exe /add-driver $infPath /install 2>&1)
$installExitCode = $LASTEXITCODE
if ($installExitCode -notin @(0, 3010)) {
    throw "pnputil failed with exit code $installExitCode`n$($installOutput -join [Environment]::NewLine)"
}

$device = Get-PnpDevice -InstanceId $instanceId
$problemCode = Get-PnpDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue
$driverVersion = Get-PnpDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_DriverVersion' -ErrorAction SilentlyContinue
$result = [pscustomobject]@{
    InstallStartedAt = $installStartedAt.ToString('o')
    InstallCompletedAt = (Get-Date).ToString('o')
    PackageVersion = '100.6.101.58030'
    InstanceId = $instanceId
    RootCertificateThumbprint = $rootCertificate.Thumbprint
    PublisherCertificateThumbprint = $publisherCertificate.Thumbprint
    PnpUtilExitCode = $installExitCode
    PnpUtilOutput = $installOutput
    DeviceStatus = $device.Status
    ProblemCode = if ($null -ne $problemCode) { $problemCode.Data } else { $null }
    DriverVersion = if ($null -ne $driverVersion) { $driverVersion.Data } else { $null }
    RemovedStaleLogs = $removedLogs
    OutputDirectory = $OutputDirectory
    RebootRequired = $true
}
$resultPath = Join-Path $OutputDirectory 'install-result.json'
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $resultPath -Encoding UTF8
$result | ConvertTo-Json -Depth 6
