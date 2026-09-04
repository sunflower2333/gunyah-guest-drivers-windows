[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\USER\viogpu-58173-c5c742f6\package',
    [string]$CertificatePath = 'C:\Users\USER\viogpu-58173-c5c742f6\DroidVM_Test.cer',
    [string]$EvidenceRoot = 'C:\Users\USER\viogpu-58173-c5c742f6\evidence',
    [string]$RollbackRoot = 'C:\Users\USER\viogpu-rollback-pre-58173-c5c742f6'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedHashes = [ordered]@{
    'viogpud3d.dll' = '7a26660fedbbe37a52d8a1235541e72817342668955f2a7bca17fbe73da8ea7b'
    'viogpud3d.pdb' = '768dcc6917119f0c06c7f62e3d230d8e1a4ff497b813ed4644eb6cf3258bd24a'
    'viogpuwddm.cat' = 'acff8e7a8aafe8ffc1d37b7f2f661bd62c2792f8e01cd86930ad17e65d72a935'
    'viogpuwddm.inf' = '26e296c2003709243bae22f4311fc00fd49f620f2b37bb9abb0df2706ebb77f0'
    'viogpuwddm.map' = 'ff1698990d46fa94f8a338295fe021862fad38e12676c2a0c357273471ba2d1a'
    'viogpuwddm.pdb' = 'fbc9f8818fe340144c63f5c325f3e7200b25d4c9c3f926a3a86965f68b357ea8'
    'viogpuwddm.sys' = '6d5705699873b9cc1061e701ddfa8246dd6143051022ce7b8b1d1cbaafbdc8dd'
}
$expectedCertificateHash = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
$expectedSignerSubject = 'CN=DroidVM Test'
$expectedVersion = '100.6.101.58173'
$expectedBeforeVersion = '100.6.101.58171'
$expectedBeforeImageHash = '3d3595baaecb92fdbf3f3b7907459dfc64978b5c2a9c57b14956040f6ff88ed0'
$expectedBeforeUmdHash = '791a9e11549a58b2df66441f7d6df36c1bf261860bca433c1aa031087e80872f'

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
    $diagnostics = [ordered]@{}
    foreach ($name in @($driverKey.GetValueNames() | Where-Object {
        $_.StartsWith('NativeContext', [StringComparison]::Ordinal)
    } | Sort-Object)) {
        $diagnostics[$name] = $driverKey.GetValue($name, $null)
    }

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
        NativeContextDiagnostics = $diagnostics
    }
}

function Assert-Package {
    if (-not (Test-Path -LiteralPath $PackageRoot -PathType Container)) {
        throw "Missing package directory: $PackageRoot"
    }
    if (-not (Test-Path -LiteralPath $CertificatePath -PathType Leaf)) {
        throw "Missing package certificate: $CertificatePath"
    }
    $certificateHash = (Get-FileHash -LiteralPath $CertificatePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($certificateHash -ne $expectedCertificateHash) {
        throw "Certificate SHA-256 mismatch: expected $expectedCertificateHash, got $certificateHash"
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
    $unexpectedFiles = @(
        Get-ChildItem -LiteralPath $PackageRoot -File |
            Where-Object { -not $expectedHashes.Contains($_.Name) }
    )
    if ($unexpectedFiles.Count -ne 0) {
        throw "Unexpected package files: $($unexpectedFiles.Name -join ', ')"
    }

    $infPath = Join-Path $PackageRoot 'viogpuwddm.inf'
    $infText = Get-Content -LiteralPath $infPath -Raw
    if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58173') {
        throw "The staged INF is not version $expectedVersion"
    }
    if ($infText -notmatch 'PCI\\VEN_1AF4&DEV_1050' -or
        $infText -match '(?i)netkvm|rdmapool|droidvmpool|DRVM0001') {
        throw 'The staged INF does not describe only the expected virtio-gpu device.'
    }

    foreach ($fileName in @('viogpuwddm.sys', 'viogpud3d.dll', 'viogpuwddm.cat')) {
        $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $PackageRoot $fileName)
        $signer = $signature.SignerCertificate
        if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
            $null -eq $signer -or
            $signer.Subject -ne $expectedSignerSubject -or
            $signer.Thumbprint -ne $certificate.Thumbprint) {
            throw "Invalid signature for '$fileName': status=$($signature.Status), signer=$($signer.Subject)"
        }
    }
    $infPath
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
    throw "Unexpected preinstall GPU state: $($before | ConvertTo-Json -Depth 6 -Compress)"
}

New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null
$before | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'install-before.json') -Encoding UTF8

New-Item -ItemType Directory -Path $RollbackRoot | Out-Null
$exportOutput = @(& pnputil.exe /export-driver $before.InfPath $RollbackRoot 2>&1)
$exportExitCode = $LASTEXITCODE
if ($exportExitCode -ne 0) {
    throw "pnputil export failed ($exportExitCode): $($exportOutput -join [Environment]::NewLine)"
}
$rollbackFiles = @(
    Get-ChildItem -LiteralPath $RollbackRoot -Recurse -File | ForEach-Object {
        [pscustomobject]@{
            Path = $_.FullName.Substring($RollbackRoot.Length).TrimStart('\')
            Length = $_.Length
            Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
)
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
$staleDiagnostics = @($before.NativeContextDiagnostics.Keys)
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
    throw "Unexpected postinstall GPU state: $($after | ConvertTo-Json -Depth 6 -Compress)"
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
