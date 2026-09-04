[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\DroidVM\viogpu-58037\viogpu',
    [string]$CertificatePath = 'C:\DroidVM\viogpu-58037\DroidVM_Test.cer',
    [string]$OutputDirectory = 'C:\DroidVM\viogpu-58037\runtime-logs',
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
    'viogpuwddm.sys' = 'f1ebee0b7adeeb6c6316892783e5bb9bac7f198da420dd264eaca8a55738309d'
    'viogpud3d.dll' = '5803a4c3d71f0953effedb2047c59f74fa39abc13d8f35cc843b83ca61d8c191'
    'viogpuwddm.cat' = '1f81200675119c74e5c5b8e4a7da80459e6edcaeeed0ad8dc883981af945ee0a'
    'viogpuwddm.inf' = '42309cca10d6936579f32efffca1279e452d976672e4664f83eed990468a8f61'
    'viogpuwddm.pdb' = '4bb41debdb53e6641945311fc288a34d7166e933b55fa07c156fe156c73f6ed3'
    'viogpuwddm.map' = '901ed3e295b93ddac5fea5812db33457a1247433fb4e81cf581985bfd9fe7e0f'
    'viogpud3d.pdb' = 'e95eaafeb93926d5a3bfa86545531b3212a9bbe6e51a0e35a61c5085a7a9034e'
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
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58037') {
    throw "The staged INF is not version 100.6.101.58037: $infPath"
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
        PackageVersion = '100.6.101.58037'
        PackageRoot = $PackageRoot
        CertificatePath = $CertificatePath
        FileCount = $expectedHashes.Count
        InstanceId = $devices[0].InstanceId
        DeviceStatus = $devices[0].Status
    } | ConvertTo-Json -Depth 3
    return
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$removedLogs = @(
    Get-ChildItem -LiteralPath $OutputDirectory -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in @('.etl', '.json', '.csv', '.xml', '.txt') } |
        Select-Object FullName, Length, LastWriteTimeUtc
)
$removedLogs | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }

$instanceId = $devices[0].InstanceId
$rootCertificate = Import-Certificate -FilePath $CertificatePath -CertStoreLocation 'Cert:\LocalMachine\Root'
$publisherCertificate = Import-Certificate -FilePath $CertificatePath -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher'
$installStartedAt = Get-Date
$installOutput = @(& pnputil.exe /add-driver $infPath /install 2>&1)
$installExitCode = $LASTEXITCODE
if ($installExitCode -notin @(0, 3010)) {
    throw "pnputil failed with exit code $installExitCode`n$($installOutput -join [Environment]::NewLine)"
}

Start-Sleep -Seconds 3
$device = Get-PnpDevice -InstanceId $instanceId
$problemCode = Get-PnpDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue
$driverVersion = Get-PnpDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_DriverVersion' -ErrorAction SilentlyContinue
$result = [pscustomobject]@{
    InstallStartedAt = $installStartedAt.ToString('o')
    InstallCompletedAt = (Get-Date).ToString('o')
    PackageVersion = '100.6.101.58037'
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
