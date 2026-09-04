[CmdletBinding()]
param(
    [string]$PackageRoot,
    [string]$CertificatePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($PackageRoot)) {
    $PackageRoot = Join-Path $PSScriptRoot 'viogpuwddm'
}
if ([string]::IsNullOrWhiteSpace($CertificatePath)) {
    $CertificatePath = Join-Path $PSScriptRoot 'DroidVM_Test.cer'
}

$expectedHashes = [ordered]@{
    'viogpuwddm.sys' = '21bb6445dffcd143bc996ee24156fd280e11cd6dd85ff393c15f0246ae74434f'
    'viogpud3d.dll' = 'e7b13323ca8d033e193dba0ef56d786265a4ec221369b746bdc240499b460188'
    'viogpuwddm.cat' = '209505ba9e532954cd6849dfccf8e436cbf00251d8fe048c196a7064eeb5bded'
}

$infPath = Join-Path $PackageRoot 'viogpuwddm.inf'
if (-not (Test-Path -LiteralPath $infPath -PathType Leaf)) {
    throw "Missing driver INF: $infPath"
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
        throw "SHA-256 mismatch for '$($entry.Key)': expected $($entry.Value), got $actual"
    }
}

$infText = Get-Content -LiteralPath $infPath -Raw
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58020') {
    throw 'The package INF is not version 100.6.101.58020.'
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
    PackageVersion = '100.6.101.58020'
    RootCertificateThumbprint = $rootCertificate.Thumbprint
    PublisherCertificateThumbprint = $publisherCertificate.Thumbprint
    PnpUtilExitCode = $installExitCode
    PnpUtilOutput = $installOutput
    DeviceBefore = $deviceBefore
    DeviceAfter = $deviceAfter
    RebootRequired = $true
} | ConvertTo-Json -Depth 5
