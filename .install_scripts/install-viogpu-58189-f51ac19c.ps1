[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\USER\viogpu-58189-f51ac19c\package',
    [string]$CertificatePath = 'C:\Users\USER\viogpu-58189-f51ac19c\DroidVM_Test.cer',
    [string]$EvidenceRoot = 'C:\Users\USER\viogpu-58189-f51ac19c\evidence',
    [string]$RollbackRoot = 'C:\Users\USER\viogpu-rollback-pre-58189-f51ac19c',
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedHashes = [ordered]@{
    'viogpud3d.dll' = 'a3f5d72f65ce6957661770ef7892027a569f18470c926accaeee44658ea6404c'
    'viogpuwddm.cat' = '9a4a1451c1e4064d1575cfc606bef48b2b37e5f8eef367955227d46d44337e3c'
    'viogpuwddm.inf' = '9c54d052e61a983574f099b2b63b79b885a02aff7f836d625eb569f14dd68005'
    'viogpuwddm.sys' = 'e77c64561ce642085e6c85ac962cf5613f6ba124942df53ac7bb0836aefd252c'
}
$expectedCertificateHash = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
$expectedVersion = '100.6.101.58189'
$expectedBeforeVersion = '100.6.101.58188'
$expectedBeforeInfPath = 'oem24.inf'
$expectedBeforeImageHash = 'b699bb08b055034fe72b1e6676993fb0d0152f663330fa9c34db0820231d40ca'
$expectedBeforeUmdHash = 'd89f3d930dfe4d0eb3480f3b7495243e7c6805eb3d94d11fc497e7c180345474'
$expectedRollbackHashes = [ordered]@{
    'viogpud3d.dll' = $expectedBeforeUmdHash
    'viogpuwddm.cat' = 'cf30d9e825cf6764331f95ffeef31960aa99917a8de6bee438fc04f71c9dfd6e'
    'viogpuwddm.inf' = 'b6a346fc4587f6802518c3a23ce3c5ab9601ed6066148fd73fcca2987e8c01c2'
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
        RenderOnly = $driverKey.GetValue('RenderOnly', $null)
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
    if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58189') { throw "The staged INF is not version $expectedVersion" }
    if ($infText -notmatch '(?im)^HKR\s*,\s*,\s*RenderOnly\s*,\s*%REG_DWORD%\s*,\s*1\s*$') { throw 'The staged INF does not default RenderOnly to 1.' }
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
        $null -eq $after.RenderOnly -or [int]$after.RenderOnly -ne 1 -or
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
