[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\USER\viogpu-58194-2141fc68\package',
    [string]$CertificatePath = 'C:\Users\USER\viogpu-58194-2141fc68\DroidVM_Test.cer',
    [string]$EvidenceRoot = 'C:\Users\USER\viogpu-58194-2141fc68\evidence',
    [string]$RollbackRoot = 'C:\Users\USER\viogpu-rollback-pre-58194-2141fc68',
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedHashes = [ordered]@{
    'viogpud3d.dll' = '3860bfeb8c9834788535241a2adaf4933fea30f71de2081d2896c24884a77736'
    'viogpuwddm.cat' = 'fc1c382b07d05450a35df713e9642cfb62a1c545e83d84e70f878513288fa9ba'
    'viogpuwddm.inf' = '5e2eacb94ed82ceeddd8f3cb122c33d38ea016ecd19752b3e68083f0755e7a8b'
    'viogpuwddm.sys' = '45ef273edf3cd4766a541ecb3952a47165735c074037eb851dc59ec753e270d4'
}
$expectedCertificateHash = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
$expectedVersion = '100.6.101.58194'
$expectedBeforeVersion = '100.6.101.58193'
$expectedBeforeInfPath = 'oem28.inf'
$expectedBeforeImageHash = '78e61f69787185fda68754842b0a46e4de8cf265e77139401b7a9b66537eaf37'
$expectedBeforeUmdHash = 'cc69318eec054240e73e4fc7c4a243c4fd33349915a361ec9a4d51f9e308e108'
$expectedRollbackHashes = [ordered]@{
    'viogpud3d.dll' = $expectedBeforeUmdHash
    'viogpuwddm.cat' = '9279c11fb3114f440cd0d033f644a54c6de8106eb8870a046d3534c1ef3d6de1'
    'viogpuwddm.inf' = 'b08152a7ad2b8f58137779661e85bdf706ea6fff48dab1b03bf6317a5e38a42c'
    'viogpuwddm.sys' = $expectedBeforeImageHash
}
$expectedSignerSubject = 'CN=DroidVM Test'

function Get-OptionalPnpData {
    param(
        [Parameter(Mandatory)][string]$InstanceId,
        [Parameter(Mandatory)][string]$KeyName
    )

    $property = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName $KeyName -ErrorAction SilentlyContinue
    if ($null -eq $property) { return $null }
    $dataProperty = $property.PSObject.Properties['Data']
    if ($null -eq $dataProperty) { return $null }
    return $dataProperty.Value
}

function Get-GpuSnapshot {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
    })
    if ($devices.Count -ne 1) { throw "Expected one present virtio-gpu device, found $($devices.Count)." }
    $device = $devices[0]
    $driverKeyName = Get-OptionalPnpData -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
    if ([string]::IsNullOrWhiteSpace([string]$driverKeyName)) { throw 'The virtio-gpu driver key is unavailable.' }
    $driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$driverKeyName"
    $driverKey = Get-Item -LiteralPath $driverKeyPath
    $serviceKey = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
    $serviceParametersPath = 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm\Parameters'
    $serviceParameters = if (Test-Path -LiteralPath $serviceParametersPath) {
        Get-Item -LiteralPath $serviceParametersPath
    } else {
        $null
    }
    $imagePath = [Environment]::ExpandEnvironmentVariables([string]$serviceKey.GetValue('ImagePath', '')).Trim('"')
    if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
        $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
    }
    $image = Get-Item -LiteralPath $imagePath
    $umd = Get-Item -LiteralPath (Join-Path $image.DirectoryName 'viogpud3d.dll')
    $imageSignature = Get-AuthenticodeSignature -LiteralPath $image.FullName
    $umdSignature = Get-AuthenticodeSignature -LiteralPath $umd.FullName
    $service = Get-CimInstance Win32_SystemDriver -Filter "Name='VioGpuWddm'"
    [pscustomobject]@{
        Status = [string]$device.Status
        Problem = if ($null -ne $device.PSObject.Properties['Problem']) { $device.Problem } else { $null }
        ProblemCode = Get-OptionalPnpData -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode'
        ProblemStatus = Get-OptionalPnpData -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemStatus'
        InstanceId = [string]$device.InstanceId
        DriverKeyPath = $driverKeyPath
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        InfPath = [string]$driverKey.GetValue('InfPath', '')
        RenderOnly = $driverKey.GetValue('RenderOnly', $null)
        ServiceRenderOnly = $serviceKey.GetValue('RenderOnly', $null)
        ServiceParametersRenderOnly = if ($null -eq $serviceParameters) { $null } else { $serviceParameters.GetValue('RenderOnly', $null) }
        ImagePath = $image.FullName
        ImageSha256 = (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        UmdPath = $umd.FullName
        UmdSha256 = (Get-FileHash -LiteralPath $umd.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        ImageSignatureStatus = [string]$imageSignature.Status
        ImageSignerSubject = if ($null -eq $imageSignature.SignerCertificate) { $null } else { $imageSignature.SignerCertificate.Subject }
        UmdSignatureStatus = [string]$umdSignature.Status
        UmdSignerSubject = if ($null -eq $umdSignature.SignerCertificate) { $null } else { $umdSignature.SignerCertificate.Subject }
        ServiceState = [string]$service.State
    }
}

function Assert-GpuHealthy {
    param([Parameter(Mandatory)]$Snapshot)

    if ($Snapshot.Status -ne 'OK' -or
        ($null -ne $Snapshot.ProblemCode -and [int]$Snapshot.ProblemCode -ne 0) -or
        ($null -ne $Snapshot.ProblemStatus -and [int64]$Snapshot.ProblemStatus -ne 0) -or
        $Snapshot.ServiceState -ne 'Running') {
        throw "Unhealthy virtio-gpu device: $($Snapshot | ConvertTo-Json -Compress)"
    }
}

function Assert-ExactDriverState {
    param(
        [Parameter(Mandatory)]$Snapshot,
        [Parameter(Mandatory)][string]$Version,
        [Parameter(Mandatory)][string]$ImageHash,
        [Parameter(Mandatory)][string]$UmdHash
    )

    Assert-GpuHealthy -Snapshot $Snapshot
    if ($Snapshot.DriverVersion -ne $Version -or
        $Snapshot.ImageSha256 -ne $ImageHash -or
        $Snapshot.UmdSha256 -ne $UmdHash -or
        $null -eq $Snapshot.RenderOnly -or [int]$Snapshot.RenderOnly -ne 1 -or
        $null -ne $Snapshot.ServiceRenderOnly -or
        $null -eq $Snapshot.ServiceParametersRenderOnly -or [int]$Snapshot.ServiceParametersRenderOnly -ne 1 -or
        $Snapshot.ImageSignatureStatus -ne 'Valid' -or
        $Snapshot.ImageSignerSubject -ne $expectedSignerSubject -or
        $Snapshot.UmdSignatureStatus -ne 'Valid' -or
        $Snapshot.UmdSignerSubject -ne $expectedSignerSubject) {
        throw "Unexpected virtio-gpu identity: $($Snapshot | ConvertTo-Json -Compress)"
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
    if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58194') { throw "The staged INF is not version $expectedVersion" }
    if ($infText -notmatch '(?im)^HKR\s*,\s*,\s*RenderOnly\s*,\s*%REG_DWORD%\s*,\s*1\s*$') { throw 'The staged INF does not default device RenderOnly to 1.' }
    if ($infText -notmatch '(?im)^HKR\s*,\s*Parameters\s*,\s*RenderOnly\s*,\s*%REG_DWORD%\s*,\s*1\s*$') { throw 'The staged INF does not default service Parameters\RenderOnly to 1.' }
    if ($infText -notmatch 'PCI\\VEN_1AF4&DEV_1050' -or $infText -match '(?i)netkvm|rdmapool|droidvmpool|DRVM0001') { throw 'The staged INF does not describe only the expected virtio-gpu device.' }
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
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'The installer must run elevated.' }
if (Test-Path -LiteralPath $RollbackRoot) { throw "Rollback directory already exists: $RollbackRoot" }
$infPath = Assert-Package
$before = Get-GpuSnapshot
Assert-ExactDriverState -Snapshot $before -Version $expectedBeforeVersion -ImageHash $expectedBeforeImageHash -UmdHash $expectedBeforeUmdHash
if ($before.InfPath -ne $expectedBeforeInfPath) {
    throw "Unexpected preinstall INF path: $($before.InfPath)"
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
$staleDiagnostics = @($driverKey.GetValueNames() | Where-Object {
    $_.StartsWith('NativeContext', [StringComparison]::Ordinal) -or
    $_.StartsWith('NativeStart', [StringComparison]::Ordinal) -or
    $_.StartsWith('NativeQueryAdapterInfo', [StringComparison]::Ordinal)
})
foreach ($name in $staleDiagnostics) { Remove-ItemProperty -LiteralPath $before.DriverKeyPath -Name $name -ErrorAction Stop }

$installStartedAt = Get-Date
$installOutput = @(& pnputil.exe /add-driver $infPath /install 2>&1)
$installExitCode = $LASTEXITCODE
if ($installExitCode -notin @(0, 3010)) { throw "pnputil install failed ($installExitCode): $($installOutput -join [Environment]::NewLine)" }
Start-Sleep -Seconds 10
$after = Get-GpuSnapshot
if ($installExitCode -eq 0) {
    Assert-ExactDriverState -Snapshot $after -Version $expectedVersion -ImageHash $expectedHashes['viogpuwddm.sys'] -UmdHash $expectedHashes['viogpud3d.dll']
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
