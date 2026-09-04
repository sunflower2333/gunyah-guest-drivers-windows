[CmdletBinding()]
param(
    [string]$BundleRoot = 'C:\DroidVM\TurnipRuns\run-33322445949\turnip-wddm-arm64-icd-bundle',
    [string]$RunnerPath = 'C:\Users\USER\run-context-diagnostic-lifecycle-once.cmd',
    [string]$OutputPrefix = 'C:\DroidVM\TurnipRuns\evidence\context-diagnostic-lifecycle-once-20260831'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedDriverVersion = '100.6.101.58180'
$expectedInfPath = 'oem20.inf'
$expectedDriverHash = 'cbaa70090a150200b6a93f078be5a74ec302d222ef149cd35d06e7598b372524'
$expectedRunnerHash = '89997ff4dc2396ec726ec55cb2b19c495a00c45d5143769a5bd8eba634451a1f'
$expectedBundleHashes = [ordered]@{
    'freedreno_icd.arm64.json' = '4344a882ad2aa543f69c8a48d152b6f3983a8349aa8a95b1c35d884989c53bb1'
    'tu_wddm_compute.spv' = '10e42787bf8e32aa36893d46c5b3d7bcabfd997e4c799116f383ff3edfc5eadf'
    'tu_wddm_graphics.frag.spv' = '803d688da968ebc017887009ec4f1234a323e8d3aaed1d1c8f6e9d0c4ca1af05'
    'tu_wddm_graphics.vert.spv' = 'e37d0dca1f6fccefef89a4b49735ed6e99e4997d806d7c5e8b4a8f2945d83646'
    'tu_wddm_kmt_probe_arm64.exe' = '5730591e852d46f360927f93889323e629b1bc13c1cbf9b6ecf9f97b7898e77b'
    'tu_wddm_vulkan_compute_probe_arm64.exe' = '62dd0a0fef2313ec12ce0036135d7acbf428a84fb3765a85445d909d15e103a5'
    'tu_wddm_vulkan_graphics_probe_arm64.exe' = '8d72e463e9b5b738fce446235691782e0e7b488a842de49a423090f5a2314081'
    'tu_wddm_vulkan_probe_arm64.exe' = '3315b4d4f765571838320eb7a276ed686e806ef0c7cc30f0336c88966f4b1b89'
    'tu_wddm_win32_probe_arm64.exe' = '848b4ae08c97d9d3d8db1408f04b865e192e8df014fd92e5f557b631184271b4'
    'TURNIP_WDDM_ICD.md' = 'caca97a6fd08d1233cc81f0091c1c3dbaa51bb31cf8c2b8d41d0ebecf3751df0'
    'turnip-wddm-icd.ps1' = 'd181e0b2b2bc10e21bae28d4363cf0ae61338ddc59e618ffcabb1ab3873603e9'
    'vulkan_freedreno.dll' = 'a1288acd874dd97c652a3b83316cf0aa69c5a2025c5a227371bcae98bb0216a5'
    'z-1.dll' = 'f8019e0161021feca20b765170c5b33a1cdc1e1bb393b858a90c294df8f71628'
}

$devices = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
})
if ($devices.Count -ne 1) {
    throw "Expected one present virtio-gpu device, found $($devices.Count)."
}

$device = $devices[0]
if ([string]$device.Status -ne 'OK') {
    throw "Virtio-gpu status is '$($device.Status)', expected 'OK'."
}

$driverProperty = Get-PnpDeviceProperty `
    -InstanceId $device.InstanceId `
    -KeyName 'DEVPKEY_Device_Driver'
$driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($driverProperty.Data)"
$driverKey = Get-Item -LiteralPath $driverKeyPath
$driverVersion = [string]$driverKey.GetValue('DriverVersion', '')
$infPath = [string]$driverKey.GetValue('InfPath', '')
if ($driverVersion -ne $expectedDriverVersion) {
    throw "Driver version is '$driverVersion', expected '$expectedDriverVersion'."
}
if (-not $infPath.Equals($expectedInfPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw "INF path is '$infPath', expected '$expectedInfPath'."
}

$service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
$driverImagePath = [Environment]::ExpandEnvironmentVariables(
    [string]$service.GetValue('ImagePath', '')
).Trim('"')
if ($driverImagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
    $driverImagePath = Join-Path $env:SystemRoot $driverImagePath.Substring(12)
} elseif (-not [IO.Path]::IsPathRooted($driverImagePath)) {
    $driverImagePath = Join-Path $env:SystemRoot $driverImagePath
}
if (-not (Test-Path -LiteralPath $driverImagePath -PathType Leaf)) {
    throw "Driver image is missing: $driverImagePath"
}
$driverHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $driverImagePath).Hash.ToLowerInvariant()
if ($driverHash -ne $expectedDriverHash) {
    throw "Loaded driver SHA-256 is '$driverHash', expected '$expectedDriverHash'."
}
$driverSignature = Get-AuthenticodeSignature -LiteralPath $driverImagePath
if ($driverSignature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
    throw "Loaded driver signature status is '$($driverSignature.Status)', expected 'Valid'."
}

if (-not (Test-Path -LiteralPath $BundleRoot -PathType Container)) {
    throw "Lifecycle bundle is missing: $BundleRoot"
}
$manifestPath = Join-Path $BundleRoot 'SHA256SUMS.txt'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Bundle manifest is missing: $manifestPath"
}

$manifestHashes = [ordered]@{}
foreach ($line in Get-Content -LiteralPath $manifestPath) {
    if ($line -notmatch '^([0-9a-f]{64})  ([^\\/]+)$') {
        throw "Malformed bundle manifest entry: $line"
    }
    if ($manifestHashes.Contains($Matches[2])) {
        throw "Duplicate bundle manifest entry: $($Matches[2])"
    }
    $manifestHashes[$Matches[2]] = $Matches[1]
}
if ($manifestHashes.Count -ne $expectedBundleHashes.Count) {
    throw "Bundle manifest has $($manifestHashes.Count) entries, expected $($expectedBundleHashes.Count)."
}

$verifiedBundleHashes = [ordered]@{}
foreach ($entry in $expectedBundleHashes.GetEnumerator()) {
    if (-not $manifestHashes.Contains($entry.Key) -or $manifestHashes[$entry.Key] -ne $entry.Value) {
        throw "Bundle manifest hash mismatch for '$($entry.Key)'."
    }
    $path = Join-Path $BundleRoot $entry.Key
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Bundle file is missing: $path"
    }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
    if ($actual -ne $entry.Value) {
        throw "Bundle file SHA-256 mismatch for '$($entry.Key)'."
    }
    $verifiedBundleHashes[$entry.Key] = $actual
}

$allowedFiles = @($expectedBundleHashes.Keys) + @('SHA256SUMS.txt')
$unexpectedFiles = @(Get-ChildItem -LiteralPath $BundleRoot -File | Where-Object {
    $_.Name -notin $allowedFiles
})
if ($unexpectedFiles.Count -ne 0) {
    throw "Unexpected bundle files: $($unexpectedFiles.Name -join ', ')"
}

if (-not (Test-Path -LiteralPath $RunnerPath -PathType Leaf)) {
    throw "One-lifecycle runner is missing: $RunnerPath"
}
$runnerHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $RunnerPath).Hash.ToLowerInvariant()
if ($runnerHash -ne $expectedRunnerHash) {
    throw "One-lifecycle runner SHA-256 is '$runnerHash', expected '$expectedRunnerHash'."
}
$evidencePaths = @(
    "$OutputPrefix.stdout.txt",
    "$OutputPrefix.stderr.txt",
    "$OutputPrefix.exit.txt"
)
foreach ($path in $evidencePaths) {
    if (Test-Path -LiteralPath $path) {
        throw "One-lifecycle evidence path is already in use: $path"
    }
}

[ordered]@{
    Ready = $true
    CollectedAt = (Get-Date).ToString('o')
    Device = [ordered]@{
        Status = [string]$device.Status
        ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) {
            $device.ProblemCode
        } else { $null }
        InstanceId = [string]$device.InstanceId
        DriverVersion = $driverVersion
        InfPath = $infPath
        DriverImagePath = $driverImagePath
        DriverImageSha256 = $driverHash
        DriverSignatureStatus = [string]$driverSignature.Status
    }
    Bundle = [ordered]@{
        Root = (Resolve-Path -LiteralPath $BundleRoot).Path
        HashCount = $verifiedBundleHashes.Count
        LifecycleExecutable = Join-Path $BundleRoot 'tu_wddm_vulkan_probe_arm64.exe'
        Hashes = $verifiedBundleHashes
    }
    Runner = [ordered]@{
        Path = $RunnerPath
        Sha256 = $runnerHash
        EvidencePaths = $evidencePaths
        EvidencePathsFree = $true
    }
} | ConvertTo-Json -Depth 6
