[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\user\viogpu-58179-635937d8\package',
    [string]$CertificatePath = 'C:\Users\user\viogpu-58179-635937d8\DroidVM_Test.cer',
    [string]$EvidenceRoot = 'C:\Users\user\viogpu-58179-635937d8\evidence',
    [string]$RollbackRoot = 'C:\Users\user\viogpu-rollback-pre-58179-635937d8'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedHashes = [ordered]@{
    'viogpud3d.dll' = 'e74666bb29c6f8375ca05f3a3b235d2a85de9659aa55814cdb9164d5c1a72fb2'
    'viogpuwddm.cat' = '214d3e6ca5c414b1c804c63323cce2b23e01a8c0930dc009252d33d672c5ac88'
    'viogpuwddm.inf' = 'f1b7986ee324802369e9e4a6117693fcb7a59ce92e8e8a46967b7e0d4b278d4a'
    'viogpuwddm.map' = '572c6ee3502133b7a260a2fcb1fd85f44033c06de9ff0e43bd29a761ae0e56f0'
    'viogpuwddm.pdb' = 'ca5e50a974f94c593430730dab10b90343ceb249b4c7971b79372b8c2ab94e9a'
    'viogpuwddm.sys' = '02c838b759a76e86f075c238f8dce49d61cd8e857f1f2dde4f55a4a28094c440'
    'viogpud3d.pdb' = '2542ef4690ac35582a24a103a2abf8f7c1cffab806b12658d3e3b1e07dc615be'
}
$expectedVersion = '100.6.101.58179'
$expectedBeforeVersion = '100.6.101.58178'
$expectedBeforeImageHash = '481ebaee0f4fb7332da3a3570919a03a6914b1e2a458bf3d1fc18b40dd7821e4'
$expectedBeforeUmdHash = 'ff78f12eb8167e354cb0b1a95604d00cf61b56bdd7bec778b8fdcd518a88e425'
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
    if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58179') { throw "The staged INF is not version $expectedVersion" }
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
New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null
$before | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'install-before.json') -Encoding UTF8
New-Item -ItemType Directory -Path $RollbackRoot | Out-Null
$exportOutput = @(& pnputil.exe /export-driver $before.InfPath $RollbackRoot 2>&1)
$exportExitCode = $LASTEXITCODE
if ($exportExitCode -ne 0) { throw "pnputil export failed ($exportExitCode): $($exportOutput -join [Environment]::NewLine)" }
$rollbackFiles = @(Get-ChildItem -LiteralPath $RollbackRoot -Recurse -File | ForEach-Object {
    [pscustomobject]@{ Path = $_.FullName.Substring($RollbackRoot.Length).TrimStart('\'); Length = $_.Length; Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
if ($rollbackFiles.Count -eq 0) { throw 'The exported rollback package is empty.' }
[ordered]@{ ExportedAt = (Get-Date).ToString('o'); PublishedInf = $before.InfPath; PnpUtilExitCode = $exportExitCode; PnpUtilOutput = $exportOutput; Files = $rollbackFiles } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $RollbackRoot 'rollback-manifest.json') -Encoding UTF8
$driverKey = Get-Item -LiteralPath $before.DriverKeyPath
$staleDiagnostics = @($driverKey.GetValueNames() | Where-Object { $_.StartsWith('NativeContext', [StringComparison]::Ordinal) })
foreach ($name in $staleDiagnostics) { Remove-ItemProperty -LiteralPath $before.DriverKeyPath -Name $name -ErrorAction Stop }
$installStartedAt = Get-Date
$installOutput = @(& pnputil.exe /add-driver $infPath /install 2>&1)
$installExitCode = $LASTEXITCODE
if ($installExitCode -notin @(0, 3010)) { throw "pnputil install failed ($installExitCode): $($installOutput -join [Environment]::NewLine)" }
Start-Sleep -Seconds 10
$after = Get-GpuSnapshot
$result = [ordered]@{ InstallStartedAt = $installStartedAt.ToString('o'); InstallCompletedAt = (Get-Date).ToString('o'); PackageVersion = $expectedVersion; PnpUtilExitCode = $installExitCode; PnpUtilOutput = $installOutput; RebootRequired = $installExitCode -eq 3010; StaleDiagnosticsRemoved = $staleDiagnostics; RollbackRoot = $RollbackRoot; RollbackFileCount = $rollbackFiles.Count; Before = $before; After = $after }
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'install-result.json') -Encoding UTF8
$result | ConvertTo-Json -Depth 8
