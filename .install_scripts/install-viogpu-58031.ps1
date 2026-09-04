[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\DroidVM\viogpu-58031\viogpuwddm',
    [string]$CertificatePath = 'C:\DroidVM\viogpu-58031\DroidVM_Test.cer',
    [string]$OutputDirectory = 'C:\DroidVM\viogpu-58031\runtime-logs',
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
    'viogpuwddm.sys' = '91f7b4b79eee01fc2a9ff735bc983337f1d28f3b15c39b57f3cc0dd41140afe2'
    'viogpud3d.dll' = 'db01277aa4eb1f760a0dd6c25d1bad5109990a71b0c6f3a7832e5a9fab4def3f'
    'viogpuwddm.cat' = 'c829a8a3d77b2d9b33f1c996a7b308c4ff83335d6059e7fe8d224c619a5eb19e'
    'viogpuwddm.inf' = '42b95363b153f76845d0bf1d86b9363ca1146b4e2964251a1704965b6eb4d168'
    'viogpuwddm.pdb' = 'ccecb1474ff4d813d6d9801369f59ee2181d3b7dd6a9123ff564014192047505'
    'viogpuwddm.map' = '8788d2decf0225a8a3fa5251eb4fa560b468dbc320c359f1719f810149e60a3f'
    'viogpud3d.pdb' = '8db8b2f7f0f59af2eed050ce4a57b09b049f4c4b047e38edd03dfbf25afe9ad5'
}
$expectedCertificateHash = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
$expectedSignerSubject = 'CN=DroidVM Test'

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
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58031') {
    throw "The staged INF is not version 100.6.101.58031: $infPath"
}
if ($infText -notmatch 'PCI\\VEN_1AF4&DEV_1050' -or $infText -match '(?i)netkvm|rdmapool|droidvmpool|DRVM0001') {
    throw 'The staged INF does not describe only the expected unprotected virtio-gpu package.'
}

foreach ($fileName in @('viogpuwddm.sys', 'viogpud3d.dll', 'viogpuwddm.cat')) {
    $signature = Get-AuthenticodeSignature -FilePath (Join-Path $PackageRoot $fileName)
    if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
        $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -ne $expectedSignerSubject) {
        throw "Invalid Authenticode signature for '$fileName': status=$($signature.Status), signer=$($signature.SignerCertificate.Subject)"
    }
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
        PackageVersion = '100.6.101.58031'
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
    58016..58030 | ForEach-Object { "C:\DroidVM\viogpu-$_\runtime-logs" }
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
    PackageVersion = '100.6.101.58031'
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
