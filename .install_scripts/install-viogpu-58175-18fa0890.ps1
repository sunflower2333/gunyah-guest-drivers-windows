[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\USER\viogpu-58175-18fa0890\package',
    [string]$CertificatePath = 'C:\Users\USER\viogpu-58175-18fa0890\DroidVM_Test.cer',
    [string]$EvidenceRoot = 'C:\Users\USER\viogpu-58175-18fa0890\evidence',
    [string]$RollbackRoot = 'C:\Users\USER\viogpu-rollback-pre-58175-18fa0890'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedHashes = [ordered]@{
    'viogpud3d.dll' = '4d4a01b2be5d43d3fe1aabddb805811641a6ca405584ec03e4e0e3487ac0980a'
    'viogpuwddm.cat' = '43a277401e23b5ce8c7fdb4d4bf4a2ad4fe9ff93972b56a8ed0e2b24ca8b11a7'
    'viogpuwddm.inf' = 'b7ce507cd634327508aa5ab9a03558aba0cadd6908df0da9238dc43e8e6f25cb'
    'viogpuwddm.map' = '8a960cf8cf117d5be4f0c1a96810f5fc8d5b426c3124f0e04ae4bc8d78fecb90'
    'viogpuwddm.pdb' = '43b4a1c72403ea1cbb8c57a9b1c8c63a3ac2fa69bc0c525bedb681c784958771'
    'viogpuwddm.sys' = '4dbb8cd63c9bcd833db62e302ab69c2dc39adddf7177d3b10ae880bcc942451a'
    'viogpud3d.pdb' = '83a84bb073d40335498621071e924043d84f36b633b199a67c1ade6771bdcb8d'
}
$expectedVersion = '100.6.101.58175'
$expectedBeforeVersion = '100.6.101.58173'
$expectedBeforeImageHash = '6d5705699873b9cc1061e701ddfa8246dd6143051022ce7b8b1d1cbaafbdc8dd'
$expectedBeforeUmdHash = '7a26660fedbbe37a52d8a1235541e72817342668955f2a7bca17fbe73da8ea7b'
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
    if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58175') {
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
