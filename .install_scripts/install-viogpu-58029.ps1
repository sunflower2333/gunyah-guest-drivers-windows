[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\DroidVM\viogpu-58029\viogpuwddm',
    [string]$CertificatePath = 'C:\DroidVM\viogpu-58029\DroidVM_Test.cer',
    [string]$OutputDirectory = 'C:\DroidVM\viogpu-58029\runtime-logs',
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
    'viogpuwddm.sys' = '9a95a119a40cc379df8fb508c0abe019139ed20a09ccbca9b29ba380b37b8d02'
    'viogpud3d.dll' = '9d601b4e48230389e451bc5e0b177b0c16e11782ecf0587d0321e428d4d6b7c0'
    'viogpuwddm.cat' = 'bd860b5993746202eb5d1efff3dc58c92a5f58d2b092785210edf8d9d52c3903'
    'viogpuwddm.inf' = 'd7cbc7d22b0d89c7bf9aef83e73a1a4d0e8905e5bc5842b2590d5deb585536f1'
    'viogpuwddm.pdb' = 'aced4c4ca59c3c8684e434c2c655edac5fed45570034fe02dc1e78e49caa5ae4'
    'viogpuwddm.map' = '073404ed9a1f9f51be322464bdc4e7c707c1246d8aa36e7767405cbae7a505ae'
    'viogpud3d.pdb' = '46c8290cd5872a883e639a32242efa1bc5c26f12fd1cd18492bb7894be0c801b'
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
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58029') {
    throw "The staged INF is not version 100.6.101.58029: $infPath"
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
        PackageVersion = '100.6.101.58029'
        PackageRoot = $PackageRoot
        CertificatePath = $CertificatePath
        FileCount = $expectedHashes.Count
        InstanceId = $devices[0].InstanceId
        DeviceStatus = $devices[0].Status
    } | ConvertTo-Json -Depth 3
    return
}

function Get-DeviceSnapshot {
    param([Parameter(Mandatory = $true)][string]$InstanceId)

    $device = Get-PnpDevice -InstanceId $InstanceId
    $properties = [ordered]@{}
    foreach ($key in @(
        'DEVPKEY_Device_ProblemCode',
        'DEVPKEY_Device_ProblemStatus',
        'DEVPKEY_Device_Driver',
        'DEVPKEY_Device_DriverVersion',
        'DEVPKEY_Device_DriverDate',
        'DEVPKEY_Device_DriverInfPath'
    )) {
        $property = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName $key -ErrorAction SilentlyContinue
        $properties[$key] = if ($null -ne $property -and $null -ne $property.PSObject.Properties['Data']) {
            $property.Data
        }
        else {
            $null
        }
    }

    return [pscustomobject]@{
        InstanceId = $device.InstanceId
        Status = $device.Status
        Class = $device.Class
        FriendlyName = $device.FriendlyName
        Properties = [pscustomobject]$properties
    }
}

$removedLogs = @()
$logExtensions = @('.etl', '.json', '.csv', '.xml', '.txt')
$logDirectories = @(
    58016..58028 | ForEach-Object { "C:\DroidVM\viogpu-$_\runtime-logs" }
) + @($OutputDirectory)
foreach ($directory in $logDirectories) {
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        continue
    }

    $logs = @(
        Get-ChildItem -LiteralPath $directory -File -ErrorAction Stop |
            Where-Object { $_.Extension -in $logExtensions }
    )
    $removedLogs += $logs | Select-Object FullName, Length, LastWriteTimeUtc
    $logs | Remove-Item -Force
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$instanceId = $devices[0].InstanceId
$deviceBefore = Get-DeviceSnapshot -InstanceId $instanceId
$rootCertificate = Import-Certificate -FilePath $CertificatePath -CertStoreLocation 'Cert:\LocalMachine\Root'
$publisherCertificate = Import-Certificate -FilePath $CertificatePath -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher'

$installStartedAt = Get-Date
$installOutput = @(& pnputil.exe /add-driver $infPath /install 2>&1)
$installExitCode = $LASTEXITCODE
if ($installExitCode -notin @(0, 3010)) {
    throw "pnputil failed with exit code $installExitCode`n$($installOutput -join [Environment]::NewLine)"
}

Start-Sleep -Seconds 3
$deviceAfter = Get-DeviceSnapshot -InstanceId $instanceId
$result = [pscustomobject]@{
    InstallStartedAt = $installStartedAt.ToString('o')
    InstallCompletedAt = (Get-Date).ToString('o')
    PackageVersion = '100.6.101.58029'
    InstanceId = $instanceId
    RootCertificateThumbprint = $rootCertificate.Thumbprint
    PublisherCertificateThumbprint = $publisherCertificate.Thumbprint
    PnpUtilExitCode = $installExitCode
    PnpUtilOutput = $installOutput
    DeviceBefore = $deviceBefore
    DeviceAfter = $deviceAfter
    RemovedStaleLogs = $removedLogs
    OutputDirectory = $OutputDirectory
    RebootRequired = $true
}
$resultPath = Join-Path $OutputDirectory 'install-result.json'
$result | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $resultPath -Encoding UTF8
$result | ConvertTo-Json -Depth 7
