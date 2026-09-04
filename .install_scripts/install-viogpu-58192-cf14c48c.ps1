[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\USER\viogpu-58192-cf14c48c\package',
    [string]$CertificatePath = 'C:\Users\USER\viogpu-58192-cf14c48c\DroidVM_Test.cer',
    [string]$EvidenceRoot = 'C:\Users\USER\viogpu-58192-cf14c48c\evidence',
    [string]$RollbackRoot = 'C:\Users\USER\viogpu-rollback-pre-58192-cf14c48c',
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedHashes = [ordered]@{
    'viogpud3d.dll' = 'efcb7b56431be589193ee67c509b182ebf7ba537d956344d873451e33936a475'
    'viogpuwddm.cat' = 'c5c11fc41a0065f6f9bb3252306ca275f47bdf39fa56c58911cdb375b9840128'
    'viogpuwddm.inf' = '399b518db79899fe997665e316c6a39c53df4b99ff5158832ddb54712dcae0e9'
    'viogpuwddm.sys' = '09afe04f9a1b83de5a40c5efa6324e664d466238219c8a2b0f5c60531be98b4d'
}
$expectedCertificateHash = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
$expectedVersion = '100.6.101.58192'
$expectedBeforeVersion = '100.6.101.58190'
$expectedBeforeInfPath = 'oem26.inf'
$expectedBeforeImageHash = '8ab40857646be6f0e40a0aec7d63bc80190f51af1d7afc5707aeae9b5bbacdf5'
$expectedBeforeUmdHash = '4aef90a8a909f19d7ab10e4874515575cfa20b8ef757c863c8717e814fa86520'
$expectedRollbackHashes = [ordered]@{
    'viogpud3d.dll' = $expectedBeforeUmdHash
    'viogpuwddm.cat' = 'b81061b897330ee3e9144b83f888936b3d32be6a61d3c3596cb61045a4d07a4d'
    'viogpuwddm.inf' = '34a57135721fb3eccc17c0393e0b10c7f695c47e699b6a4e8a8957d4c13a2207'
    'viogpuwddm.sys' = $expectedBeforeImageHash
}
$expectedSignerSubject = 'CN=DroidVM Test'

function Get-GpuSnapshot {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
    })
    if ($devices.Count -ne 1) { throw "Expected one present virtio-gpu device, found $($devices.Count)." }
    $device = $devices[0]
    $problemCode = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode'
    $problemStatus = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemStatus'
    $property = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
    $driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($property.Data)"
    $driverKey = Get-Item -LiteralPath $driverKeyPath
    $service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
    $serviceParametersPath = 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm\Parameters'
    $serviceParameters = if (Test-Path -LiteralPath $serviceParametersPath) {
        Get-Item -LiteralPath $serviceParametersPath
    } else {
        $null
    }
    $imagePath = [Environment]::ExpandEnvironmentVariables([string]$service.GetValue('ImagePath', '')).Trim('"')
    if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
        $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
    }
    $image = Get-Item -LiteralPath $imagePath
    $umdPath = Join-Path $image.DirectoryName 'viogpud3d.dll'
    $signature = Get-AuthenticodeSignature -LiteralPath $image.FullName
    $serviceState = Get-CimInstance Win32_SystemDriver -Filter "Name='VioGpuWddm'"
    [pscustomobject]@{
        Status = [string]$device.Status
        Problem = if ($null -ne $device.PSObject.Properties['Problem']) { $device.Problem } else { $null }
        ProblemCode = $problemCode.Data
        ProblemStatus = $problemStatus.Data
        InstanceId = [string]$device.InstanceId
        DriverKeyPath = $driverKeyPath
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        InfPath = [string]$driverKey.GetValue('InfPath', '')
        RenderOnly = $driverKey.GetValue('RenderOnly', $null)
        ServiceRenderOnly = $service.GetValue('RenderOnly', $null)
        ServiceParametersRenderOnly = if ($null -eq $serviceParameters) { $null } else { $serviceParameters.GetValue('RenderOnly', $null) }
        ImagePath = $image.FullName
        ImageSha256 = (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        UmdPath = $umdPath
        UmdSha256 = if (Test-Path -LiteralPath $umdPath -PathType Leaf) { (Get-FileHash -LiteralPath $umdPath -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
        SignatureStatus = [string]$signature.Status
        SignerSubject = if ($null -eq $signature.SignerCertificate) { $null } else { $signature.SignerCertificate.Subject }
        ServiceState = [string]$serviceState.State
    }
}

function Assert-GpuHealthy {
    param([Parameter(Mandatory)]$Snapshot)

    if ($Snapshot.Status -ne 'OK' -or ($null -ne $Snapshot.ProblemCode -and [int]$Snapshot.ProblemCode -ne 0)) {
        throw "Unhealthy virtio-gpu device: $($Snapshot | ConvertTo-Json -Compress)"
    }
}

function Assert-KnownRecoverableState {
    param([Parameter(Mandatory)]$Snapshot)

    if ($Snapshot.Status -ne 'Error' -or
        [int]$Snapshot.ProblemCode -ne 43 -or
        [int64]$Snapshot.ProblemStatus -ne 0 -or
        $Snapshot.ServiceState -ne 'Stopped') {
        throw "Refusing recovery from an unexpected GPU state: $($Snapshot | ConvertTo-Json -Compress)"
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
    if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58192') { throw "The staged INF is not version $expectedVersion" }
    if ($infText -notmatch '(?im)^HKR\s*,\s*,\s*RenderOnly\s*,\s*%REG_DWORD%\s*,\s*1\s*$') { throw 'The staged INF does not default device RenderOnly to 1.' }
    if ($infText -notmatch '(?im)^HKR\s*,\s*Parameters\s*,\s*RenderOnly\s*,\s*%REG_DWORD%\s*,\s*1\s*$') { throw 'The staged INF does not default service Parameters\RenderOnly to 1.' }
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
Assert-KnownRecoverableState -Snapshot $before
if ($before.DriverVersion -ne $expectedBeforeVersion -or
    $before.InfPath -ne $expectedBeforeInfPath -or
    $null -eq $before.RenderOnly -or [int]$before.RenderOnly -ne 1 -or
    $null -ne $before.ServiceRenderOnly -or
    $null -ne $before.ServiceParametersRenderOnly -or
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
        $null -eq $after.ServiceParametersRenderOnly -or [int]$after.ServiceParametersRenderOnly -ne 1 -or
        $after.SignatureStatus -ne 'Valid' -or
        $after.SignerSubject -ne $expectedSignerSubject -or
        $after.ServiceState -ne 'Running') {
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
