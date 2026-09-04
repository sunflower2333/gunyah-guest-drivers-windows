[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Baseline', 'Final')]
    [string]$Mode,
    [string]$BaselinePath = 'C:\DroidVM\TurnipRuns\evidence\viogpu-58186-c913fd87-stability-baseline.json',
    [string]$OutputPath = 'C:\DroidVM\TurnipRuns\evidence\viogpu-58186-c913fd87-stability-final.json',
    [string]$KmtSummaryPath = 'C:\DroidVM\TurnipRuns\evidence\viogpu-58186-c913fd87-kmt\kmt-summary.json',
    [string]$TurnipSummaryPath = 'C:\DroidVM\TurnipRuns\evidence\viogpu-58186-c913fd87-turnip\turnip-summary.json',
    [string]$NegativeSummaryPath = 'C:\DroidVM\TurnipRuns\evidence\viogpu-58186-c913fd87-negative\negative-summary.json',
    [int]$MinimumObservationMinutes = 30
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedDriverVersion = '100.6.101.58186'
$expectedDriverHash = 'd3f920cb6a5367b468b831fd10d80308fd1272a5e2f32dc29a8a863442a91be2'
$expectedUmdHash = 'a65d1abeec1860fe9a8f8be58a53e6f71f8a635659dcbd42bb71b98b8a452754'
$expectedSignerSubject = 'CN=DroidVM Test'
$destroyValueSuffixes = @(
    'Attempt', 'Status', 'Detail', 'HostResult', 'ContextId',
    'ContextState', 'OwnerState', 'Released', 'Retrying', 'OwnerRetained'
)

function ConvertTo-U32 {
    param([object]$Value)
    [int64]$signed = [Convert]::ToInt64($Value)
    if ($signed -lt 0) { return [uint64]($signed + 0x100000000L) }
    return [uint64]$signed
}

function Get-GpuSnapshot {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
    })
    if ($devices.Count -ne 1) { throw "Expected one present virtio-gpu device, found $($devices.Count)." }
    $device = $devices[0]
    $property = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
    $driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($property.Data)"
    $driverKey = Get-Item -LiteralPath $driverKeyPath
    $serviceKey = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
    $imagePath = [Environment]::ExpandEnvironmentVariables([string]$serviceKey.GetValue('ImagePath', '')).Trim('"')
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
        $diagnostics[$name] = $driverKey.GetValue(
            $name,
            $null,
            [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames
        )
    }
    [pscustomobject]@{
        Status = [string]$device.Status
        ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) { $device.ProblemCode } else { $null }
        InstanceId = [string]$device.InstanceId
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        DriverImagePath = $image.FullName
        DriverImageSha256 = (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        UmdPath = $umdPath
        UmdSha256 = (Get-FileHash -LiteralPath $umdPath -Algorithm SHA256).Hash.ToLowerInvariant()
        SignatureStatus = [string]$signature.Status
        SignerSubject = if ($null -eq $signature.SignerCertificate) { $null } else { $signature.SignerCertificate.Subject }
        DriverServiceStatus = [string](Get-Service -Name VioGpuWddm).Status
        NativeContextDiagnostics = $diagnostics
    }
}

function Test-GpuSnapshot {
    param([object]$Snapshot, [string]$ExpectedInstanceId = '')
    $errors = [System.Collections.Generic.List[string]]::new()
    if ($Snapshot.Status -ne 'OK') { $errors.Add("Device status is '$($Snapshot.Status)', expected 'OK'.") }
    if ($null -ne $Snapshot.ProblemCode -and [int]$Snapshot.ProblemCode -ne 0) {
        $errors.Add("Device problem code is '$($Snapshot.ProblemCode)', expected 0.")
    }
    if ($Snapshot.DriverVersion -ne $expectedDriverVersion) { $errors.Add("Unexpected driver version '$($Snapshot.DriverVersion)'.") }
    if ($Snapshot.DriverImageSha256 -ne $expectedDriverHash) { $errors.Add("Unexpected SYS SHA-256 '$($Snapshot.DriverImageSha256)'.") }
    if ($Snapshot.UmdSha256 -ne $expectedUmdHash) { $errors.Add("Unexpected UMD SHA-256 '$($Snapshot.UmdSha256)'.") }
    if ($Snapshot.SignatureStatus -ne 'Valid' -or $Snapshot.SignerSubject -ne $expectedSignerSubject) {
        $errors.Add("Unexpected driver signature '$($Snapshot.SignatureStatus)' by '$($Snapshot.SignerSubject)'.")
    }
    if ($Snapshot.DriverServiceStatus -ne 'Running') { $errors.Add("Driver service is '$($Snapshot.DriverServiceStatus)'.") }
    if (-not [string]::IsNullOrEmpty($ExpectedInstanceId) -and $Snapshot.InstanceId -ne $ExpectedInstanceId) {
        $errors.Add("Device instance changed from '$ExpectedInstanceId' to '$($Snapshot.InstanceId)'.")
    }
    [pscustomobject]@{ Passed = $errors.Count -eq 0; Errors = @($errors) }
}

function Test-DestroyDiagnostics {
    param([System.Collections.Specialized.OrderedDictionary]$Values)
    $errors = [System.Collections.Generic.List[string]]::new()
    $stageNames = @($Values.Keys | Where-Object {
        $_ -match '^NativeContextDestroySlot[0-9]{2}Stage$'
    } | Sort-Object)
    if ($stageNames.Count -eq 0) { $errors.Add('No committed Native Context destroy slot was recorded.') }
    foreach ($stageName in $stageNames) {
        $prefix = $stageName.Substring(0, $stageName.Length - 'Stage'.Length)
        $missing = @($destroyValueSuffixes | Where-Object {
            -not $Values.Contains($prefix + $_)
        })
        if ($missing.Count -ne 0) {
            $errors.Add("Destroy slot '$prefix' is missing fields: $($missing -join ', ').")
            continue
        }
        $slot = [ordered]@{}
        foreach ($suffix in $destroyValueSuffixes) {
            $slot[$suffix] = ConvertTo-U32 $Values[$prefix + $suffix]
        }
        $stage = ConvertTo-U32 $Values[$prefix + 'Stage']
        if ($slot.Attempt -eq 0 -or $stage -ne 0x0FFF -or $slot.Status -ne 0 -or
            $slot.Detail -ne 0 -or $slot.HostResult -ne 1 -or $slot.ContextState -ne 4 -or
            $slot.OwnerState -ne 2 -or $slot.Released -ne 1 -or $slot.Retrying -ne 0 -or
            $slot.OwnerRetained -ne 0) {
            $errors.Add("Destroy slot '$prefix' is not a clean terminal record.")
        }
    }
    [pscustomobject]@{ Passed = $errors.Count -eq 0; SlotCount = $stageNames.Count; Errors = @($errors) }
}

function Get-SuiteEvidence {
    $evidence = @()
    foreach ($entry in ([ordered]@{
        Kmt = $KmtSummaryPath
        Turnip = $TurnipSummaryPath
        Negative = $NegativeSummaryPath
    }).GetEnumerator()) {
        if (-not (Test-Path -LiteralPath $entry.Value -PathType Leaf)) { throw "Missing $($entry.Key) summary: $($entry.Value)" }
        $summary = Get-Content -LiteralPath $entry.Value -Raw | ConvertFrom-Json
        if ($null -eq $summary.PSObject.Properties['Success'] -or -not [bool]$summary.Success) {
            throw "$($entry.Key) summary does not report success."
        }
        $evidence += [pscustomobject]@{
            Name = [string]$entry.Key
            Path = [string](Resolve-Path -LiteralPath $entry.Value)
            Sha256 = (Get-FileHash -LiteralPath $entry.Value -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $evidence
}

function Get-DumpInventory {
    $items = @()
    foreach ($root in @('C:\Windows\Minidump', 'C:\Windows\LiveKernelReports')) {
        $items += @(Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
            ForEach-Object {
                [pscustomobject]@{
                    Path = $_.FullName
                    Length = $_.Length
                    LastWriteTimeUtc = $_.LastWriteTimeUtc.ToString('o')
                }
            })
    }
    @($items | Sort-Object Path)
}

function Get-CriticalEvents {
    param([datetime]$Since)
    $system = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $Since } -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -in @(14, 17, 18, 19, 41, 1001, 4101, 6008) } |
        Select-Object TimeCreated, Id, ProviderName, LevelDisplayName, Message)
    $application = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $Since } -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Id -in @(1000, 1001) -and
            $_.ProviderName -match 'Desktop Window Manager|Application Error|Windows Error Reporting'
        } | Select-Object TimeCreated, Id, ProviderName, LevelDisplayName, Message)
    @($system + $application)
}

if ($MinimumObservationMinutes -lt 1) { throw 'MinimumObservationMinutes must be at least 1.' }
$gpu = Get-GpuSnapshot
$gpuCheck = Test-GpuSnapshot $gpu
$destroyCheck = Test-DestroyDiagnostics $gpu.NativeContextDiagnostics
$suite = @(Get-SuiteEvidence)
$bootTime = ([datetime](Get-CimInstance Win32_OperatingSystem).LastBootUpTime).ToUniversalTime()

if ($Mode -eq 'Baseline') {
    if (Test-Path -LiteralPath $BaselinePath) { throw "Stability baseline already exists: $BaselinePath" }
    if (-not $gpuCheck.Passed -or -not $destroyCheck.Passed) {
        throw "Baseline health failed: $(@($gpuCheck.Errors + $destroyCheck.Errors) -join ' ')"
    }
    $baseline = [ordered]@{
        Success = $true
        CollectedAt = (Get-Date).ToUniversalTime().ToString('o')
        LastBootUpTimeUtc = $bootTime.ToString('o')
        MinimumObservationMinutes = $MinimumObservationMinutes
        Gpu = $gpu
        GpuCheck = $gpuCheck
        DestroyCheck = $destroyCheck
        SuiteEvidence = $suite
        DumpInventory = @(Get-DumpInventory)
    }
    New-Item -ItemType Directory -Path (Split-Path $BaselinePath) -Force | Out-Null
    [IO.File]::WriteAllText($BaselinePath, ($baseline | ConvertTo-Json -Depth 12))
    $baseline | ConvertTo-Json -Depth 12
    exit 0
}

if (-not (Test-Path -LiteralPath $BaselinePath -PathType Leaf)) { throw "Missing stability baseline: $BaselinePath" }
if (Test-Path -LiteralPath $OutputPath) { throw "Stability final evidence already exists: $OutputPath" }
$baseline = Get-Content -LiteralPath $BaselinePath -Raw | ConvertFrom-Json
$errors = [System.Collections.Generic.List[string]]::new()
if (-not [bool]$baseline.Success) { $errors.Add('Baseline does not report success.') }
$baselineTime = [datetime]$baseline.CollectedAt
$elapsedMinutes = ((Get-Date).ToUniversalTime() - $baselineTime.ToUniversalTime()).TotalMinutes
$requiredObservationMinutes = [math]::Max($MinimumObservationMinutes, [int]$baseline.MinimumObservationMinutes)
if ($elapsedMinutes -lt $requiredObservationMinutes) {
    $errors.Add("Observation window is $([math]::Round($elapsedMinutes, 2)) minutes, expected at least $requiredObservationMinutes.")
}
if ($bootTime.ToString('o') -ne ([datetime]$baseline.LastBootUpTimeUtc).ToUniversalTime().ToString('o')) {
    $errors.Add('Windows rebooted after the stability baseline.')
}
$gpuCheck = Test-GpuSnapshot $gpu ([string]$baseline.Gpu.InstanceId)
foreach ($message in @($gpuCheck.Errors + $destroyCheck.Errors)) { $errors.Add($message) }
$baselineSuite = @($baseline.SuiteEvidence | ForEach-Object { "$($_.Name)|$($_.Path)|$($_.Sha256)" })
$currentSuite = @($suite | ForEach-Object { "$($_.Name)|$($_.Path)|$($_.Sha256)" })
if (@(Compare-Object $baselineSuite $currentSuite).Count -ne 0) { $errors.Add('Suite evidence changed after the stability baseline.') }
$baselineDumps = @($baseline.DumpInventory | ForEach-Object { "$($_.Path)|$($_.Length)|$($_.LastWriteTimeUtc)" })
$currentDumpInventory = @(Get-DumpInventory)
$currentDumps = @($currentDumpInventory | ForEach-Object { "$($_.Path)|$($_.Length)|$($_.LastWriteTimeUtc)" })
if (@(Compare-Object $baselineDumps $currentDumps).Count -ne 0) { $errors.Add('Crash-dump inventory changed after the stability baseline.') }
$events = @(Get-CriticalEvents $baselineTime.AddMilliseconds(1))
if ($events.Count -ne 0) { $errors.Add("Found $($events.Count) new critical system/application events.") }
$result = [ordered]@{
    Success = $errors.Count -eq 0
    CollectedAt = (Get-Date).ToUniversalTime().ToString('o')
    BaselinePath = $BaselinePath
    ObservationMinutes = $elapsedMinutes
    MinimumObservationMinutes = $requiredObservationMinutes
    LastBootUpTimeUtc = $bootTime.ToString('o')
    Gpu = $gpu
    GpuCheck = $gpuCheck
    DestroyCheck = $destroyCheck
    SuiteEvidence = $suite
    DumpInventory = $currentDumpInventory
    CriticalEvents = $events
    Errors = @($errors)
}
New-Item -ItemType Directory -Path (Split-Path $OutputPath) -Force | Out-Null
$json = $result | ConvertTo-Json -Depth 12
[IO.File]::WriteAllText($OutputPath, $json)
$json
if (-not $result.Success) { exit 1 }
exit 0
