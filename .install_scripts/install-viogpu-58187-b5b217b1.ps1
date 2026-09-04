[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\USER\viogpu-58187-b5b217b1\package',
    [string]$CertificatePath = 'C:\Users\USER\viogpu-58187-b5b217b1\DroidVM_Test.cer',
    [string]$EvidenceRoot = 'C:\Users\USER\viogpu-58187-b5b217b1\evidence',
    [string]$RollbackRoot = 'C:\Users\USER\viogpu-rollback-pre-58187-b5b217b1',
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedHashes = [ordered]@{
    'viogpud3d.dll' = 'adc830091dc35049b6b4adf5c07d16e8f284b5983787f93d08c7a89b8d898a53'
    'viogpuwddm.cat' = 'dd290fb7ee8c8bce9d104bc0107dd9b1730bfa3536076572871fa4b108496091'
    'viogpuwddm.inf' = '1b882c8e0e3b7705e20c522f114667cba68411e207a8a552177d5de033845f3b'
    'viogpuwddm.sys' = 'd0320f7bedd739232f761a256d42b11f12f92b6a98551c2140e6efe27ef7543d'
}
$expectedCertificateHash = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
$expectedVersion = '100.6.101.58187'
$expectedBeforeVersion = '100.6.101.58186'
$expectedBeforeInfPath = 'oem22.inf'
$expectedBeforeImageHash = 'd3f920cb6a5367b468b831fd10d80308fd1272a5e2f32dc29a8a863442a91be2'
$expectedBeforeUmdHash = 'a65d1abeec1860fe9a8f8be58a53e6f71f8a635659dcbd42bb71b98b8a452754'
$expectedRollbackHashes = [ordered]@{
    'viogpud3d.dll' = $expectedBeforeUmdHash
    'viogpuwddm.cat' = '8577d47b015d0f466facbdbe419d8e6497959cec05d6e2d67c99f30312fdb6a7'
    'viogpuwddm.inf' = '338819b9f408150e9788b1d425109544bde0de60cc5902494c565e74823b5db7'
    'viogpuwddm.sys' = $expectedBeforeImageHash
}
$expectedSignerSubject = 'CN=DroidVM Test'

function Get-GpuSnapshot {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
    })
    if ($devices.Count -ne 1) { throw "Expected one present virtio-gpu device, found $($devices.Count)." }
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
        UmdSha256 = if (Test-Path -LiteralPath $umdPath -PathType Leaf) { (Get-FileHash -LiteralPath $umdPath -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
        SignatureStatus = [string]$signature.Status
        SignerSubject = if ($null -eq $signature.SignerCertificate) { $null } else { $signature.SignerCertificate.Subject }
    }
}

function Assert-GpuHealthy {
    param([Parameter(Mandatory)]$Snapshot)

    if ($Snapshot.Status -ne 'OK' -or ($null -ne $Snapshot.ProblemCode -and [int]$Snapshot.ProblemCode -ne 0)) {
        throw "Unhealthy virtio-gpu device: $($Snapshot | ConvertTo-Json -Compress)"
    }
}

function Assert-Package {
    if (-not (Test-Path -LiteralPath $PackageRoot -PathType Container)) { throw "Missing package directory: $PackageRoot" }
    if (-not (Test-Path -LiteralPath $CertificatePath -PathType Leaf)) { throw "Missing package certificate: $CertificatePath" }
    $certificateHash = (Get-FileHash -LiteralPath $CertificatePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($certificateHash -ne $expectedCertificateHash) {
        throw "Certificate SHA-256 mismatch: expected $expectedCertificateHash, got $certificateHash"
    }
    $certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($CertificatePath)
    foreach ($entry in $expectedHashes.GetEnumerator()) {
        $path = Join-Path $PackageRoot $entry.Key
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing package file: $path" }
        $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $entry.Value) { throw "SHA-256 mismatch for '$path': expected $($entry.Value), got $actual" }
    }
    $unexpected = @(Get-ChildItem -LiteralPath $PackageRoot -File | Where-Object { -not $expectedHashes.Contains($_.Name) })
    if ($unexpected.Count -ne 0) { throw "Unexpected package files: $($unexpected.Name -join ', ')" }
    $infPath = Join-Path $PackageRoot 'viogpuwddm.inf'
    $infText = Get-Content -LiteralPath $infPath -Raw
    if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58187') { throw "The staged INF is not version $expectedVersion" }
    if ($infText -notmatch 'PCI\\VEN_1AF4&DEV_1050' -or $infText -match '(?i)netkvm|rdmapool|droidvmpool|DRVM0001') { throw 'The staged INF does not describe only the expected virtio-gpu device.' }
    foreach ($fileName in @('viogpuwddm.sys', 'viogpud3d.dll', 'viogpuwddm.cat')) {
        $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $PackageRoot $fileName)
        if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or $null -eq $signature.SignerCertificate -or $signature.SignerCertificate.Subject -ne $expectedSignerSubject -or $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) {
            $subject = if ($null -eq $signature.SignerCertificate) { '<none>' } else { $signature.SignerCertificate.Subject }
            throw "Invalid signature for '$fileName': status=$($signature.Status), signer=$subject"
        }
    }
    return $infPath
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'The installer must run elevated.' }
if (Test-Path -LiteralPath $RollbackRoot) { throw "Rollback directory already exists: $RollbackRoot" }
$infPath = Assert-Package
$before = Get-GpuSnapshot
Assert-GpuHealthy -Snapshot $before
if ($before.DriverVersion -ne $expectedBeforeVersion -or
    $before.InfPath -ne $expectedBeforeInfPath -or
    $before.ImageSha256 -ne $expectedBeforeImageHash -or
    $before.UmdSha256 -ne $expectedBeforeUmdHash -or
    $before.SignatureStatus -ne 'Valid' -or
    $before.SignerSubject -ne $expectedSignerSubject) {
    throw "Unexpected preinstall GPU state: $($before | ConvertTo-Json -Compress)"
}
if ($ValidateOnly) {
    [ordered]@{
        ValidationOnly = $true
        PackageVersion = $expectedVersion
        RollbackRootAbsent = $true
        Before = $before
    } | ConvertTo-Json -Depth 6
    return
}

New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null
$before | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'install-before.json') -Encoding UTF8
New-Item -ItemType Directory -Path $RollbackRoot | Out-Null
$exportOutput = @(& pnputil.exe /export-driver $before.InfPath $RollbackRoot 2>&1)
$exportExitCode = $LASTEXITCODE
if ($exportExitCode -ne 0) { throw "pnputil export failed ($exportExitCode): $($exportOutput -join [Environment]::NewLine)" }
$rollbackFiles = @(Get-ChildItem -LiteralPath $RollbackRoot -Recurse -File | ForEach-Object {
    [pscustomobject]@{
        Name = $_.Name
        Path = $_.FullName.Substring($RollbackRoot.Length).TrimStart('\')
        Length = $_.Length
        Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
if ($rollbackFiles.Count -eq 0) { throw 'The exported rollback package is empty.' }
foreach ($entry in $expectedRollbackHashes.GetEnumerator()) {
    $matches = @($rollbackFiles | Where-Object { $_.Name -ieq $entry.Key })
    if ($matches.Count -ne 1) { throw "Expected exactly one rollback '$($entry.Key)', found $($matches.Count)." }
    if ($matches[0].Sha256 -ne $entry.Value) {
        throw "Rollback SHA-256 mismatch for '$($entry.Key)': expected $($entry.Value), got $($matches[0].Sha256)"
    }
}
$unexpectedRollback = @($rollbackFiles | Where-Object { -not $expectedRollbackHashes.Contains($_.Name) })
if ($unexpectedRollback.Count -ne 0) { throw "Unexpected rollback files: $($unexpectedRollback.Name -join ', ')" }
[ordered]@{
    ExportedAt = (Get-Date).ToString('o')
    PublishedInf = $before.InfPath
    PnpUtilExitCode = $exportExitCode
    PnpUtilOutput = $exportOutput
    Files = $rollbackFiles
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $RollbackRoot 'rollback-manifest.json') -Encoding UTF8

$driverKey = Get-Item -LiteralPath $before.DriverKeyPath
$staleDiagnostics = @($driverKey.GetValueNames() | Where-Object { $_.StartsWith('NativeContext', [StringComparison]::Ordinal) })
foreach ($name in $staleDiagnostics) { Remove-ItemProperty -LiteralPath $before.DriverKeyPath -Name $name -ErrorAction Stop }

$installStartedAt = Get-Date
$installOutput = @(& pnputil.exe /add-driver $infPath /install 2>&1)
$installExitCode = $LASTEXITCODE
if ($installExitCode -notin @(0, 3010)) { throw "pnputil install failed ($installExitCode): $($installOutput -join [Environment]::NewLine)" }
Start-Sleep -Seconds 10
$after = Get-GpuSnapshot
if ($installExitCode -eq 0) {
    Assert-GpuHealthy -Snapshot $after
    if ($after.DriverVersion -ne $expectedVersion -or
        $after.ImageSha256 -ne $expectedHashes['viogpuwddm.sys'] -or
        $after.UmdSha256 -ne $expectedHashes['viogpud3d.dll'] -or
        $after.SignatureStatus -ne 'Valid' -or
        $after.SignerSubject -ne $expectedSignerSubject) {
        throw "The installed GPU state does not match ${expectedVersion}: $($after | ConvertTo-Json -Compress)"
    }
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
