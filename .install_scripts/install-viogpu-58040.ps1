[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\DroidVM\viogpu-58040\viogpu',
    [string]$CertificatePath = 'C:\DroidVM\viogpu-58040\DroidVM_Test.cer',
    [string]$OutputDirectory = 'C:\DroidVM\viogpu-58040\runtime-logs'
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
    'viogpuwddm.sys' = '98f54eab847998e6ee94826f88c83c8d24846f6cb8411e5d690a5920e26e8b47'
    'viogpud3d.dll' = '8f559d81065e377aaeca54725352a55b9e9541412743cbc7c090f45856c990a9'
    'viogpuwddm.cat' = 'ec812f48eff8f2d000cc574c6d474cd0e4dcd94da1ca8f15be048bc1ff4ffa62'
    'viogpuwddm.inf' = '4a71cbcb9ab987604486a913b1f17d070380a9bfe2427430a5f930edbc51fd86'
    'viogpuwddm.pdb' = 'a8f973873b26b51b904eacba846afaa50730a0cf421fb728a1e32918a776736d'
    'viogpuwddm.map' = '0a9692e32cef28a5eaa58d5d4d47d931952934aa39c3ed4b0ae2e29e941ab7a7'
    'viogpud3d.pdb' = 'd2179cec65f3cc69f9f00096e64528da3396bfa97ed77f2f51bf7b5343897e7f'
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
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58040') {
    throw "The staged INF is not version 100.6.101.58040: $infPath"
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

Start-Sleep -Seconds 5
$device = Get-PnpDevice -InstanceId $instanceId
$problemCode = Get-PnpDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue
$driverVersion = Get-PnpDeviceProperty -InstanceId $instanceId -KeyName 'DEVPKEY_Device_DriverVersion' -ErrorAction SilentlyContinue
$result = [pscustomobject]@{
    InstallStartedAt = $installStartedAt.ToString('o')
    InstallCompletedAt = (Get-Date).ToString('o')
    PackageVersion = '100.6.101.58040'
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
}
$resultPath = Join-Path $OutputDirectory 'install-result.json'
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $resultPath -Encoding UTF8
$result | ConvertTo-Json -Depth 6
