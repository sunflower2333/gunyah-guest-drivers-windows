[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\USER\viogpu-58185-47f202e8\package',
    [string]$CertificatePath = 'C:\Users\USER\viogpu-58185-47f202e8\DroidVM_Test.cer',
    [string]$EvidenceRoot = 'C:\Users\USER\viogpu-58185-47f202e8\evidence',
    [string]$RollbackRoot = 'C:\Users\USER\viogpu-rollback-pre-58185-47f202e8',
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedHashes = [ordered]@{
    'viogpud3d.dll' = 'f05abc28ad38a8f99b414cd991cea28b0155e257b4e30d03b9af3374eb8b715d'
    'viogpud3d.pdb' = '0d035ab621e4f8655d87e26c847a51c0016676069b97e046e59536958dfeef42'
    'viogpuwddm.cat' = 'a2e7305ce933ac00e754491d42b4daecc7d5ddf18abc53005449d5c82b11b1dc'
    'viogpuwddm.inf' = 'b4a647826f90035f561c17804eac8f28f6d358531d549af90cb5e48e6d849910'
    'viogpuwddm.map' = 'fc663ce9fff8ea5beb4071bba3d4910724f5ac24ec9803bae1799493d021f90a'
    'viogpuwddm.pdb' = '98545efb7578d0950c44999326044c374f78a6824f1bf840c3846362cfd0bd25'
    'viogpuwddm.sys' = '32f0f3c0fcd9f5e61fe8db0d86d88d8989930276868385cd638d2aa5e494c372'
}
$expectedCertificateHash = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
$expectedVersion = '100.6.101.58185'
$expectedBeforeVersion = '100.6.101.58182'
$expectedBeforeImageHash = 'a6c1009691958bec6df629752f12beb190a95cc8e0d138548247bd8e0ade7229'
$expectedBeforeUmdHash = 'bd862478c4dbf81da0204f2004fb96f2f1d005ab38bdfcde3d794fedfd28bb3e'
$expectedRollbackHashes = [ordered]@{
    'viogpud3d.dll' = $expectedBeforeUmdHash
    'viogpuwddm.cat' = 'ada59e6f4b6aede076bd661a591d9490396472ed2fc92c3b5cd1d654eb33c2eb'
    'viogpuwddm.inf' = '91438a009c932da66e4ae232f864488c7aedfab8d08cc499bc8dfa4108a9eec9'
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
    if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58185') { throw "The staged INF is not version $expectedVersion" }
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
if ($before.DriverVersion -ne $expectedBeforeVersion -or $before.ImageSha256 -ne $expectedBeforeImageHash -or $before.UmdSha256 -ne $expectedBeforeUmdHash -or $before.SignatureStatus -ne 'Valid') {
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
if ($installExitCode -eq 0 -and
    ($after.DriverVersion -ne $expectedVersion -or
     $after.ImageSha256 -ne $expectedHashes['viogpuwddm.sys'] -or
     $after.UmdSha256 -ne $expectedHashes['viogpud3d.dll'] -or
     $after.SignatureStatus -ne 'Valid')) {
    throw "The installed GPU state does not match ${expectedVersion}: $($after | ConvertTo-Json -Compress)"
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
