[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\DroidVM\viogpu-58022\viogpuwddm',
    [string]$CertificatePath = 'C:\DroidVM\viogpu-58022\DroidVM_Test.cer'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedHashes = [ordered]@{
    'viogpuwddm.sys' = 'c964825ebb2e5793e3928c43bbff51d5d0078e28d10db226ee3f8a24aee69960'
    'viogpud3d.dll' = 'd4d1a85603ff3b083aa74e8f08407f0cf07e061196d257ab75fd598c6e82119e'
    'viogpuwddm.cat' = 'd226cf0fc7ef339b506f1289aaf6e89efc6a6ac58fabdbbc888174d9d5a2842b'
    'viogpuwddm.inf' = 'cf67ad7dd6b97cb488d4484e346a41a419544e1d1137f89302fea933e54b909b'
}

if (-not (Test-Path -LiteralPath $CertificatePath -PathType Leaf)) {
    throw "Missing test certificate: $CertificatePath"
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

$infPath = Join-Path $PackageRoot 'viogpuwddm.inf'
$infText = Get-Content -LiteralPath $infPath -Raw
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58022') {
    throw "The staged INF is not version 100.6.101.58022: $infPath"
}

$staleLogRoots = @(
    'C:\DroidVM\viogpu-58016\runtime-logs',
    'C:\DroidVM\viogpu-58017\runtime-logs',
    'C:\DroidVM\viogpu-58018\runtime-logs',
    'C:\DroidVM\viogpu-58019\runtime-logs',
    'C:\DroidVM\viogpu-58020\runtime-logs',
    'C:\DroidVM\viogpu-58021\runtime-logs'
)
$removedLogs = @()
foreach ($root in $staleLogRoots) {
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        continue
    }

    $logs = @(
        Get-ChildItem -LiteralPath $root -File -ErrorAction Stop |
            Where-Object { $_.Extension -in @('.etl', '.json', '.csv', '.xml') }
    )
    $removedLogs += $logs | Select-Object FullName, Length, LastWriteTimeUtc
    $logs | Remove-Item -Force
}

$rootCertificate = Import-Certificate -FilePath $CertificatePath -CertStoreLocation 'Cert:\LocalMachine\Root'
$publisherCertificate = Import-Certificate -FilePath $CertificatePath -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher'

$deviceBefore = @(
    Get-PnpDevice -PresentOnly |
        Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' } |
        Select-Object Status, Class, FriendlyName, InstanceId
)

$installStartedAt = Get-Date
$installOutput = @(& pnputil.exe /add-driver $infPath /install 2>&1)
$installExitCode = $LASTEXITCODE
if ($installExitCode -notin @(0, 3010)) {
    throw "pnputil failed with exit code $installExitCode`n$($installOutput -join [Environment]::NewLine)"
}

Start-Sleep -Seconds 3
$deviceAfter = @(
    Get-PnpDevice -PresentOnly |
        Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' } |
        Select-Object Status, Class, FriendlyName, InstanceId
)

[pscustomobject]@{
    InstallStartedAt = $installStartedAt.ToString('o')
    InstallCompletedAt = (Get-Date).ToString('o')
    PackageVersion = '100.6.101.58022'
    RootCertificateThumbprint = $rootCertificate.Thumbprint
    PublisherCertificateThumbprint = $publisherCertificate.Thumbprint
    PnpUtilExitCode = $installExitCode
    PnpUtilOutput = $installOutput
    DeviceBefore = $deviceBefore
    DeviceAfter = $deviceAfter
    RemovedStaleLogs = $removedLogs
    RebootRequired = $true
} | ConvertTo-Json -Depth 5
