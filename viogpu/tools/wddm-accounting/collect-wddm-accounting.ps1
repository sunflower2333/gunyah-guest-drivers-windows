[CmdletBinding()]
param(
    [string]$ProbePath = "$PSScriptRoot/wddm-accounting.exe",
    [Parameter(Mandatory)][ValidatePattern('^0x[0-9a-fA-F]{8}_0x[0-9a-fA-F]{8}$')][string]$AdapterLuid,
    [Parameter(Mandatory)][string]$WorkloadPath,
    # Exact Windows command-line arguments; the executable path is separate.
    [string]$WorkloadArguments = '',
    [ValidateRange(2, 30)][int]$IdleSeconds = 5,
    [ValidateRange(1, 60)][int]$WorkloadTimeoutSeconds = 15,
    [switch]$KmtStatistics,
    [string]$OutputDirectory = (Join-Path (Get-Location) ('wddm-accounting-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')))
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/accounting-deltas.ps1"
$ProbePath = (Resolve-Path -LiteralPath $ProbePath).Path
$WorkloadPath = (Resolve-Path -LiteralPath $WorkloadPath).Path
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Output directory already exists; choose a fresh capture directory' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
[void](New-Item -ItemType Directory -Path $OutputDirectory)
$rawPath = Join-Path $OutputDirectory 'samples.jsonl'
$script:previousEngines = @{}
$script:previousMemory = @{}
$script:sampleNumber = 0
$script:captureErrors = [Collections.Generic.List[string]]::new()
$script:knownEngineNames = @{}
$startedAt = Get-Date
$workload = $null
$workloadStarted = $false
$workloadPid = 0
$timedOut = $false
$workloadExitCode = $null
$stdoutTask = $null
$stderrTask = $null

function Get-DesktopProcesses {
    @(Get-Process dwm, explorer -ErrorAction SilentlyContinue | Select-Object Id, ProcessName)
}

function Read-PerformanceRows([string]$ClassName) {
    try {
        $rows = @(Get-CimInstance -ClassName $ClassName -OperationTimeoutSec 3 -ErrorAction Stop |
            Where-Object { $_.Name -like "*$AdapterLuid*" })
        return [pscustomobject]@{ error = $null; rows = $rows }
    } catch {
        $message = "${ClassName}: $($_.Exception.Message)"
        $script:captureErrors.Add($message)
        return [pscustomobject]@{ error = $message; rows = @() }
    }
}

function Save-Sample([string]$Phase, [int]$TargetPid) {
    $arguments = @('--luid', $AdapterLuid)
    if ($TargetPid -gt 0) { $arguments += @('--pid', [string]$TargetPid) }
    if ($KmtStatistics) { $arguments += '--kmt-statistics' }
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $ProbePath
    $info.Arguments = $arguments -join ' ' # Only validated LUID, numeric PID and fixed flags.
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $probe = [Diagnostics.Process]::Start($info)
    $read = $probe.StandardOutput.ReadToEndAsync()
    $nativeText = ''
    $nativeExit = 2
    try {
        if ($probe.WaitForExit(5000)) {
            $nativeExit = $probe.ExitCode
            if ($read.Wait(1000)) { $nativeText = $read.Result }
        } else {
            $probe.Kill()
            [void]$probe.WaitForExit(1000)
            $script:captureErrors.Add('Native snapshot exceeded 5-second deadline')
        }
    } finally { $probe.Dispose() }
    try { $native = $nativeText | ConvertFrom-Json } catch { $native = $null; $script:captureErrors.Add('Native snapshot JSON could not be parsed') }
    if ($nativeExit -ne 0) { $script:captureErrors.Add("Native snapshot exit code $nativeExit") }
    $engines = Read-PerformanceRows 'Win32_PerfRawData_GPUPerformanceCounters_GPUEngine'
    $memory = Read-PerformanceRows 'Win32_PerfRawData_GPUPerformanceCounters_GPUAdapterMemory'
    $processMemory = Read-PerformanceRows 'Win32_PerfRawData_GPUPerformanceCounters_GPUProcessMemory'
    $engineRows = @()
    $currentNames = @{}
    foreach ($row in $engines.rows) {
        $name = [string]$row.Name
        $currentNames[$name] = $true
        $script:knownEngineNames[$name] = $true
        $raw = [pscustomobject]@{ Name = $name; RunningTime = $row.RunningTime; Timestamp_Sys100NS = $row.Timestamp_Sys100NS }
        $engineRows += [pscustomobject]@{ raw = $raw; delta = (Get-EngineAccountingDelta $script:previousEngines[$name] $raw) }
        $script:previousEngines[$name] = $raw
    }
    # Do not interpolate across a missing instance or failed provider query.
    foreach ($name in @($script:previousEngines.Keys)) {
        if (-not $currentNames.ContainsKey($name)) { $script:previousEngines.Remove($name) }
    }
    $memoryRows = @()
    $currentNames = @{}
    foreach ($row in $memory.rows) {
        $name = [string]$row.Name
        $currentNames[$name] = $true
        $raw = [pscustomobject]@{ Name = $name; DedicatedUsage = $row.DedicatedUsage; SharedUsage = $row.SharedUsage; TotalCommitted = $row.TotalCommitted }
        $memoryRows += [pscustomobject]@{ raw = $raw; delta = (Get-MemoryAccountingDelta $script:previousMemory[$name] $raw) }
        $script:previousMemory[$name] = $raw
    }
    foreach ($name in @($script:previousMemory.Keys)) {
        if (-not $currentNames.ContainsKey($name)) { $script:previousMemory.Remove($name) }
    }
    $entry = [ordered]@{
        sample = $script:sampleNumber++; phase = $Phase; utc = [DateTime]::UtcNow.ToString('o'); workload_pid = $TargetPid
        native_exit = $nativeExit; native = $native; engine_error = $engines.error; engines = $engineRows
        adapter_memory_error = $memory.error; adapter_memory = $memoryRows
        process_memory_error = $processMemory.error
        process_memory = @($processMemory.rows | Select-Object Name, DedicatedUsage, SharedUsage, TotalCommitted, Timestamp_Sys100NS)
    }
    $entry | ConvertTo-Json -Depth 16 -Compress | Add-Content -LiteralPath $rawPath -Encoding UTF8
}

function Capture-Idle([string]$Phase) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do { Save-Sample $Phase 0; Start-Sleep -Milliseconds 1000 } while ($timer.Elapsed.TotalSeconds -lt $IdleSeconds)
}

$before = Get-DesktopProcesses
$failure = $null
try {
    Capture-Idle 'idle_before'
    # Verify exact adapter selection before starting any workload.
    $initial = Get-Content -LiteralPath $rawPath -TotalCount 1 | ConvertFrom-Json
    if (-not $initial.native.selection_found -or $initial.native_exit -ne 0) { throw 'Selected adapter could not be captured' }
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $WorkloadPath
    $startInfo.Arguments = $WorkloadArguments
    $startInfo.WorkingDirectory = Split-Path -Parent $WorkloadPath
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $workload = [Diagnostics.Process]::new()
    $workload.StartInfo = $startInfo
    if (-not $workload.Start()) { throw 'Workload failed to start' }
    $workloadStarted = $true
    $workloadPid = $workload.Id
    $stdoutTask = $workload.StandardOutput.ReadToEndAsync()
    $stderrTask = $workload.StandardError.ReadToEndAsync()
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        Save-Sample 'workload' $workloadPid
        if ($workload.WaitForExit(1000)) { break }
    } while ($timer.Elapsed.TotalSeconds -lt $WorkloadTimeoutSeconds)
    if (-not $workload.HasExited) {
        $timedOut = $true
        $workload.Kill() # Only the exact process object launched by this script.
        if (-not $workload.WaitForExit(5000)) { throw 'Owned workload did not exit after timeout termination' }
    }
    $workloadExitCode = $workload.ExitCode
    Capture-Idle 'idle_after'
} catch {
    $failure = $_.Exception.Message
    $script:captureErrors.Add($failure)
} finally {
    if ($null -ne $workload) {
        if ($workloadStarted) {
            if (-not $workload.HasExited) { $timedOut = $true; $workload.Kill(); [void]$workload.WaitForExit(5000) }
            foreach ($item in @(@{ task = $stdoutTask; name = 'workload.stdout.txt' }, @{ task = $stderrTask; name = 'workload.stderr.txt' })) {
                try {
                    if ($null -ne $item.task -and $item.task.Wait(5000)) {
                        [IO.File]::WriteAllText((Join-Path $OutputDirectory $item.name), $item.task.Result)
                    } else { $script:captureErrors.Add("Workload output incomplete: $($item.name)") }
                } catch { $script:captureErrors.Add("Workload output error: $($_.Exception.Message)") }
            }
        }
        $workload.Dispose()
    }
}
$summary = [ordered]@{
    schema = 'viogpu_wddm2_capture_v1'; started = $startedAt.ToUniversalTime().ToString('o'); ended = [DateTime]::UtcNow.ToString('o')
    adapter_luid = $AdapterLuid; probe_hash = (Get-FileHash -LiteralPath $ProbePath -Algorithm SHA256).Hash
    workload_path = $WorkloadPath; workload_arguments = $WorkloadArguments
    workload_hash = (Get-FileHash -LiteralPath $WorkloadPath -Algorithm SHA256).Hash
    workload_pid = $workloadPid; workload_exit_code = $workloadExitCode; workload_timed_out = $timedOut
    kmt_reserved_statistics_opt_in = [bool]$KmtStatistics
    desktop_before = $before; desktop_after = (Get-DesktopProcesses)
    engine_instances_observed = @($script:knownEngineNames.Keys); samples = $script:sampleNumber
    collection_errors = @($script:captureErrors.ToArray()); validation = 'requires_review_of_phase_deltas_completed_output_and_UI'
}
$summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding UTF8
Write-Host "Captured $($script:sampleNumber) samples to $OutputDirectory; workload exit=$workloadExitCode timeout=$timedOut. No automatic GPU pass decision."
if ($failure -or $timedOut -or $workloadExitCode -ne 0 -or $script:captureErrors.Count -gt 0) { exit 2 }
