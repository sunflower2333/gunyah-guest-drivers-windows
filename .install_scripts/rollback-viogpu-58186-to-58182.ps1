[CmdletBinding()]
param(
    [string]$RollbackRoot = 'C:\Users\USER\viogpu-rollback-pre-58186-c913fd87',
    [string]$EvidenceRoot = 'C:\Users\USER\viogpu-58186-c913fd87\evidence',
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedCurrentVersion = '100.6.101.58186'
$expectedCurrentImageHash = 'd3f920cb6a5367b468b831fd10d80308fd1272a5e2f32dc29a8a863442a91be2'
$expectedCurrentUmdHash = 'a65d1abeec1860fe9a8f8be58a53e6f71f8a635659dcbd42bb71b98b8a452754'
$expectedRollbackVersion = '100.6.101.58182'
$expectedRollbackHashes = [ordered]@{
    'viogpud3d.dll' = 'bd862478c4dbf81da0204f2004fb96f2f1d005ab38bdfcde3d794fedfd28bb3e'
    'viogpuwddm.cat' = 'ada59e6f4b6aede076bd661a591d9490396472ed2fc92c3b5cd1d654eb33c2eb'
    'viogpuwddm.inf' = '91438a009c932da66e4ae232f864488c7aedfab8d08cc499bc8dfa4108a9eec9'
    'viogpuwddm.sys' = 'a6c1009691958bec6df629752f12beb190a95cc8e0d138548247bd8e0ade7229'
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
    $serviceStatus = (Get-Service -Name VioGpuWddm).Status
    [pscustomobject]@{
        Status = [string]$device.Status
        ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) { $device.ProblemCode } else { $null }
        InstanceId = [string]$device.InstanceId
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        InfPath = [string]$driverKey.GetValue('InfPath', '')
        ImagePath = $image.FullName
        ImageSha256 = (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        UmdPath = $umdPath
        UmdSha256 = if (Test-Path -LiteralPath $umdPath -PathType Leaf) { (Get-FileHash -LiteralPath $umdPath -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
        SignatureStatus = [string]$signature.Status
        SignerSubject = if ($null -eq $signature.SignerCertificate) { $null } else { $signature.SignerCertificate.Subject }
        DriverServiceStatus = [string]$serviceStatus
    }
}

function Test-GpuSnapshot {
    param(
        [object]$Snapshot,
        [string]$Version,
        [string]$ImageHash,
        [string]$UmdHash,
        [string]$ExpectedInstanceId = ''
    )

    $errors = [System.Collections.Generic.List[string]]::new()
    if ($Snapshot.Status -ne 'OK') { $errors.Add("Device status is '$($Snapshot.Status)', expected 'OK'.") }
    if ($null -ne $Snapshot.ProblemCode -and [int]$Snapshot.ProblemCode -ne 0) {
        $errors.Add("Device problem code is '$($Snapshot.ProblemCode)', expected 0.")
    }
    if ($Snapshot.DriverVersion -ne $Version) { $errors.Add("Driver version is '$($Snapshot.DriverVersion)', expected '$Version'.") }
    if ($Snapshot.ImageSha256 -ne $ImageHash) { $errors.Add("SYS SHA-256 is '$($Snapshot.ImageSha256)', expected '$ImageHash'.") }
    if ($Snapshot.UmdSha256 -ne $UmdHash) { $errors.Add("UMD SHA-256 is '$($Snapshot.UmdSha256)', expected '$UmdHash'.") }
    if ($Snapshot.SignatureStatus -ne 'Valid' -or $Snapshot.SignerSubject -ne $expectedSignerSubject) {
        $errors.Add("Driver signature is '$($Snapshot.SignatureStatus)' by '$($Snapshot.SignerSubject)'.")
    }
    if ($Snapshot.DriverServiceStatus -ne 'Running') {
        $errors.Add("VioGpuWddm service is '$($Snapshot.DriverServiceStatus)', expected 'Running'.")
    }
    if (-not [string]::IsNullOrEmpty($ExpectedInstanceId) -and $Snapshot.InstanceId -ne $ExpectedInstanceId) {
        $errors.Add("Device instance changed from '$ExpectedInstanceId' to '$($Snapshot.InstanceId)'.")
    }
    [pscustomobject]@{ Passed = $errors.Count -eq 0; Errors = @($errors) }
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Rollback must run elevated.' }
if (-not (Test-Path -LiteralPath $RollbackRoot -PathType Container)) { throw "Missing rollback directory: $RollbackRoot" }

$resolvedRoot = (Resolve-Path -LiteralPath $RollbackRoot).Path.TrimEnd('\')
$manifestPath = Join-Path $resolvedRoot 'rollback-manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "Missing rollback manifest: $manifestPath" }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$files = @($manifest.Files)
if ($files.Count -ne $expectedRollbackHashes.Count) {
    throw "Rollback manifest contains $($files.Count) files, expected $($expectedRollbackHashes.Count)."
}
$manifestNames = @($files | ForEach-Object { [string]$_.Name })
foreach ($expectedName in $expectedRollbackHashes.Keys) {
    if (@($manifestNames | Where-Object { $_ -ieq $expectedName }).Count -ne 1) {
        throw "Rollback manifest does not contain exactly one '$expectedName'."
    }
}
$unexpectedNames = @($manifestNames | Where-Object { -not $expectedRollbackHashes.Contains($_) })
if ($unexpectedNames.Count -ne 0) { throw "Rollback manifest contains unexpected files: $($unexpectedNames -join ', ')" }

$verifiedFiles = @()
foreach ($entry in $files) {
    $relativePath = [string]$entry.Path
    if ([string]::IsNullOrWhiteSpace($relativePath) -or [IO.Path]::IsPathRooted($relativePath)) {
        throw "Unsafe rollback path in manifest: '$relativePath'"
    }
    $path = [IO.Path]::GetFullPath((Join-Path $resolvedRoot $relativePath))
    if (-not $path.StartsWith($resolvedRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Rollback path escapes root: '$relativePath'"
    }
    $file = Get-Item -LiteralPath $path
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($file.Length -ne [long]$entry.Length -or $hash -ne [string]$entry.Sha256 -or $hash -ne $expectedRollbackHashes[[string]$entry.Name]) {
        throw "Rollback manifest mismatch for '$relativePath'"
    }
    $verifiedFiles += $file
}

$infFiles = @($verifiedFiles | Where-Object { $_.Extension -ieq '.inf' })
if ($infFiles.Count -ne 1) { throw "Expected one rollback INF, found $($infFiles.Count)." }
$infPath = $infFiles[0].FullName
$infText = Get-Content -LiteralPath $infPath -Raw
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58182') { throw "Rollback INF is not version $expectedRollbackVersion" }
if ($infText -notmatch 'PCI\\VEN_1AF4&DEV_1050' -or $infText -match '(?i)netkvm|rdmapool|droidvmpool|DRVM0001') { throw 'Rollback INF does not describe only the expected virtio-gpu device.' }
foreach ($fileName in @('viogpuwddm.sys', 'viogpud3d.dll', 'viogpuwddm.cat')) {
    $signedFile = @($verifiedFiles | Where-Object { $_.Name -ieq $fileName })
    if ($signedFile.Count -ne 1) { throw "Expected exactly one signed rollback '$fileName'." }
    $signature = Get-AuthenticodeSignature -LiteralPath $signedFile[0].FullName
    if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
        $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -ne $expectedSignerSubject) {
        throw "Invalid rollback signature for '$fileName'."
    }
}

$before = Get-GpuSnapshot
$beforeCheck = Test-GpuSnapshot $before $expectedCurrentVersion $expectedCurrentImageHash $expectedCurrentUmdHash
if (-not $beforeCheck.Passed) { throw "Unexpected pre-rollback GPU state: $($beforeCheck.Errors -join ' ')" }
if ($ValidateOnly) {
    [ordered]@{
        ValidationOnly = $true
        RollbackVersion = $expectedRollbackVersion
        VerifiedFileCount = $verifiedFiles.Count
        RollbackInf = $infPath
        Before = $before
        BeforeCheck = $beforeCheck
    } | ConvertTo-Json -Depth 6
    return
}

$resultPath = Join-Path $EvidenceRoot 'rollback-58186-to-58182-result.json'
if (Test-Path -LiteralPath $resultPath) { throw "Rollback evidence already exists: $resultPath" }

$rollbackStartedAt = Get-Date
$rollbackOutput = @(& pnputil.exe /add-driver $infPath /install 2>&1)
$rollbackExitCode = $LASTEXITCODE
if ($rollbackExitCode -notin @(0, 3010)) { throw "pnputil rollback failed ($rollbackExitCode): $($rollbackOutput -join [Environment]::NewLine)" }
Start-Sleep -Seconds 10
$after = Get-GpuSnapshot
$afterCheck = Test-GpuSnapshot $after $expectedRollbackVersion $expectedRollbackHashes['viogpuwddm.sys'] $expectedRollbackHashes['viogpud3d.dll'] $before.InstanceId
$success = $rollbackExitCode -eq 0 -and $afterCheck.Passed
$result = [ordered]@{
    Success = $success
    RollbackStartedAt = $rollbackStartedAt.ToString('o')
    RollbackCompletedAt = (Get-Date).ToString('o')
    RollbackVersion = $expectedRollbackVersion
    PnpUtilExitCode = $rollbackExitCode
    PnpUtilOutput = $rollbackOutput
    RebootRequired = $rollbackExitCode -eq 3010
    VerifiedFileCount = $verifiedFiles.Count
    Before = $before
    BeforeCheck = $beforeCheck
    After = $after
    AfterCheck = $afterCheck
}
New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null
$json = $result | ConvertTo-Json -Depth 8
$json | Set-Content -LiteralPath $resultPath -Encoding UTF8
$json
if (-not $success) { exit 1 }
exit 0
