[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\USER\viogpu-58178-463b56b9\package',
    [string]$CertificatePath = 'C:\Users\USER\viogpu-58178-463b56b9\DroidVM_Test.cer',
    [string]$EvidenceRoot = 'C:\Users\USER\viogpu-58178-463b56b9\evidence',
    [string]$RollbackRoot = 'C:\Users\USER\viogpu-rollback-pre-58178-463b56b9'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedHashes = [ordered]@{
    'viogpud3d.dll' = 'ff78f12eb8167e354cb0b1a95604d00cf61b56bdd7bec778b8fdcd518a88e425'
    'viogpuwddm.cat' = '32e4e140721562572ed945751fa76ecb67db196bcc57cc4db7286cdf7dd14d9c'
    'viogpuwddm.inf' = '93abcdc966fc9bbc774a4eb08ef98f153ceaea80681dff015212cf488e4f146c'
    'viogpuwddm.map' = 'bb5286dc1d4183cbe91dbb112a8d19903d37f0f2b86f6678a28ae1acb04b88be'
    'viogpuwddm.pdb' = '024542c47ad3a90f5db02d1897670de60b8a27cec9e50777279833247218409d'
    'viogpuwddm.sys' = '481ebaee0f4fb7332da3a3570919a03a6914b1e2a458bf3d1fc18b40dd7821e4'
    'viogpud3d.pdb' = 'fcd1805eba578bc8731776dc1e1dca2b9e2b7f917cd66d997ffb4636fb7a43dc'
}
$expectedVersion = '100.6.101.58178'
$expectedBeforeVersion = '100.6.101.58177'
$expectedBeforeImageHash = 'f98d0c306be1b1d36f5550be79e83ae2cd12ebbf9f3efd913792c9dc9cb9dd77'
$expectedBeforeUmdHash = '195695f043d77038737c0b9de856135cd062a5dfe54627bb1f66805a9a43f17a'
$expectedSignerSubject = 'CN=DroidVM Test'

function Get-GpuSnapshot {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
    })
    if ($devices.Count -ne 1) {
        throw "Expected one present virtio-gpu device, found $($devices.Count)."
    }

    $device = $devices[0]
    $property = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
    $driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($property.Data)"
    $driverKey = Get-Item -LiteralPath $driverKeyPath
    $service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
    $imagePath = [Environment]::ExpandEnvironmentVariables([string]$service.GetValue('ImagePath', '')).Trim('"')
    if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
        $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
    }
    $image = Get-Item -LiteralPath $imagePath
    $umdPath = Join-Path $image.DirectoryName 'viogpud3d.dll'
    $signature = Get-AuthenticodeSignature -LiteralPath $image.FullName

    [pscustomobject]@{
        Status = [string]$device.Status
        ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) { $device.ProblemCode } else { $null }
        InstanceId = [string]$device.InstanceId
        DriverKeyPath = $driverKeyPath
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        InfPath = [string]$driverKey.GetValue('InfPath', '')
        ImagePath = $image.FullName
        ImageSha256 = (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        UmdPath = $umdPath
        UmdSha256 = if (Test-Path -LiteralPath $umdPath -PathType Leaf) {
            (Get-FileHash -LiteralPath $umdPath -Algorithm SHA256).Hash.ToLowerInvariant()
        } else { $null }
        SignatureStatus = [string]$signature.Status
        SignerSubject = if ($null -eq $signature.SignerCertificate) { $null } else { $signature.SignerCertificate.Subject }
    }
}

function Assert-Package {
    if (-not (Test-Path -LiteralPath $PackageRoot -PathType Container)) {
        throw "Missing package directory: $PackageRoot"
    }
    if (-not (Test-Path -LiteralPath $CertificatePath -PathType Leaf)) {
        throw "Missing package certificate: $CertificatePath"
    }
    $certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($CertificatePath)
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
    $unexpected = @(Get-ChildItem -LiteralPath $PackageRoot -File | Where-Object {
        -not $expectedHashes.Contains($_.Name)
    })
    if ($unexpected.Count -ne 0) {
        throw "Unexpected package files: $($unexpected.Name -join ', ')"
    }
    $infPath = Join-Path $PackageRoot 'viogpuwddm.inf'
    $infText = Get-Content -LiteralPath $infPath -Raw
    if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58178') {
        throw "The staged INF is not version $expectedVersion"
    }
    if ($infText -notmatch 'PCI\\VEN_1AF4&DEV_1050' -or $infText -match '(?i)netkvm|rdmapool|droidvmpool|DRVM0001') {
        throw 'The staged INF does not describe only the expected virtio-gpu device.'
    }
    foreach ($fileName in @('viogpuwddm.sys', 'viogpud3d.dll', 'viogpuwddm.cat')) {
        $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $PackageRoot $fileName)
        if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
            $null -eq $signature.SignerCertificate -or
            $signature.SignerCertificate.Subject -ne $expectedSignerSubject -or
            $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) {
            $subject = if ($null -eq $signature.SignerCertificate) { '<none>' } else { $signature.SignerCertificate.Subject }
            throw "Invalid signature for '$fileName': status=$($signature.Status), signer=$subject"
        }
    }
    return $infPath
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'The viogpu package must be installed from an elevated PowerShell session.'
}
if (Test-Path -LiteralPath $RollbackRoot) {
    throw "Rollback directory already exists: $RollbackRoot"
}

$infPath = Assert-Package
$before = Get-GpuSnapshot
if ($before.Status -ne 'OK' -or
    ($null -ne $before.ProblemCode -and $before.ProblemCode -ne 0) -or
    $before.DriverVersion -ne $expectedBeforeVersion -or
    $before.ImageSha256 -ne $expectedBeforeImageHash -or
    $before.UmdSha256 -ne $expectedBeforeUmdHash -or
    $before.SignatureStatus -ne 'Valid') {
    throw "Unexpected preinstall GPU state: $($before | ConvertTo-Json -Compress)"
}

New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null
$before | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'install-before.json') -Encoding UTF8
New-Item -ItemType Directory -Path $RollbackRoot | Out-Null
$exportOutput = @(& pnputil.exe /export-driver $before.InfPath $RollbackRoot 2>&1)
$exportExitCode = $LASTEXITCODE
if ($exportExitCode -ne 0) {
    throw "pnputil export failed ($exportExitCode): $($exportOutput -join [Environment]::NewLine)"
}
$rollbackFiles = @(Get-ChildItem -LiteralPath $RollbackRoot -Recurse -File | ForEach-Object {
    [pscustomobject]@{
        Path = $_.FullName.Substring($RollbackRoot.Length).TrimStart('\')
        Length = $_.Length
        Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
if ($rollbackFiles.Count -eq 0) {
    throw 'The exported rollback package is empty.'
}
$rollbackManifest = [ordered]@{
    ExportedAt = (Get-Date).ToString('o')
    PublishedInf = $before.InfPath
    PnpUtilExitCode = $exportExitCode
    PnpUtilOutput = $exportOutput
    Files = $rollbackFiles
}
$rollbackManifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $RollbackRoot 'rollback-manifest.json') -Encoding UTF8

$driverKey = Get-Item -LiteralPath $before.DriverKeyPath
$staleDiagnostics = @($driverKey.GetValueNames() | Where-Object {
    $_.StartsWith('NativeContext', [StringComparison]::Ordinal)
})
foreach ($name in $staleDiagnostics) {
    Remove-ItemProperty -LiteralPath $before.DriverKeyPath -Name $name -ErrorAction Stop
}

$installStartedAt = Get-Date
$installOutput = @(& pnputil.exe /add-driver $infPath /install 2>&1)
$installExitCode = $LASTEXITCODE
if ($installExitCode -notin @(0, 3010)) {
    throw "pnputil install failed ($installExitCode): $($installOutput -join [Environment]::NewLine)"
}
Start-Sleep -Seconds 10
$after = Get-GpuSnapshot
if ($after.DriverVersion -ne $expectedVersion -or
    $after.Status -ne 'OK' -or
    ($null -ne $after.ProblemCode -and $after.ProblemCode -ne 0) -or
    $after.ImageSha256 -ne $expectedHashes['viogpuwddm.sys'] -or
    $after.UmdSha256 -ne $expectedHashes['viogpud3d.dll'] -or
    $after.SignatureStatus -ne 'Valid' -or
    $after.SignerSubject -ne $expectedSignerSubject) {
    throw "Unexpected postinstall GPU state: $($after | ConvertTo-Json -Compress)"
}

$result = [ordered]@{
    InstallStartedAt = $installStartedAt.ToString('o')
    InstallCompletedAt = (Get-Date).ToString('o')
    PackageVersion = $expectedVersion
    PnpUtilExitCode = $installExitCode
    PnpUtilOutput = $installOutput
    RebootRequired = $installExitCode -eq 3010
    StaleDiagnosticsRemoved = $staleDiagnostics
    RollbackRoot = $RollbackRoot
    RollbackFileCount = $rollbackFiles.Count
    Before = $before
    After = $after
}
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'install-result.json') -Encoding UTF8
$result | ConvertTo-Json -Depth 8
