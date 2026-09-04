[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\DroidVM\viogpu-58036\viogpu',
    [string]$CertificatePath = 'C:\DroidVM\viogpu-58036\DroidVM_Test.cer',
    [string]$OutputDirectory = 'C:\DroidVM\viogpu-58036\runtime-logs',
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
    'viogpuwddm.sys' = 'd3e3c22af26f40e64a646973f905ac52be55f3f1825f039217eb9eaec0f99daf'
    'viogpud3d.dll' = '6ba8d403bb225b222a4181e03bb8f7303f44367a8c34c09f58489e3c791a72dc'
    'viogpuwddm.cat' = 'bc845450b730ffa6b1711adad039bc013e30a27cec69642c459dc02c9e99ddd0'
    'viogpuwddm.inf' = '513cf54e5c1cfab8407bf62fab055b86a2eb7634a30c1f786d212b4bf253bb4f'
    'viogpuwddm.pdb' = '409cc6aefb960cc4b7b7bb4a5d292e8cd158caaa2270da930a04810e766b1f7c'
    'viogpuwddm.map' = 'bd1d7642a9369644b206ba42acb71affcdf43d508ca27d1374ab571296930df0'
    'viogpud3d.pdb' = 'fd0bb8d182aafbcbd209ab145ae53b0b48e6ee47c2774e053ad029762e6dbcd1'
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
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58036') {
    throw "The staged INF is not version 100.6.101.58036: $infPath"
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
        PackageVersion = '100.6.101.58036'
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
    58016..58035 | ForEach-Object { "C:\DroidVM\viogpu-$_\runtime-logs" }
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
    PackageVersion = '100.6.101.58036'
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
