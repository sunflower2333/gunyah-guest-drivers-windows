$ErrorActionPreference = 'Continue'
Set-StrictMode -Version Latest

$expectedTrace = 'DroidVM-VioGpu-58173-SubmitNop-20260830-031954799'
$candidatePids = @(828, 6788, 7120)
$before = @(
    Get-CimInstance Win32_Process |
        Where-Object { $_.ProcessId -in $candidatePids -or $_.Name -eq 'tu_wddm_kmt_probe_arm64.exe' } |
        Select-Object ProcessId, ParentProcessId, Name, CommandLine, CreationDate
)
$actions = @()
foreach ($row in $before) {
    $command = [string]$row.CommandLine
    $isProbe = $row.Name -eq 'tu_wddm_kmt_probe_arm64.exe' -and
        $command -like '*viogpu-58173-c5c742f6*kmt*tu_wddm_kmt_probe_arm64.exe*--stage=allocation*--submit-nop*'
    $isTraceControl = $row.Name -eq 'logman.exe' -and
        (($command -like "*$expectedTrace*") -or ($command -match '(?i)logman\s+query\s+-ets'))
    if (-not ($isProbe -or $isTraceControl)) {
        continue
    }
    Stop-Process -Id ([int]$row.ProcessId) -Force -ErrorAction SilentlyContinue
    $actions += [pscustomobject]@{
        ProcessId = [int]$row.ProcessId
        Name = [string]$row.Name
        CommandLine = $command
        Stopped = -not (Get-Process -Id ([int]$row.ProcessId) -ErrorAction SilentlyContinue)
    }
}
Start-Sleep -Milliseconds 500
$submit = 'C:\Users\USER\viogpu-58173-c5c742f6\kmt-submit-nop'
$files = @(
    Get-ChildItem -LiteralPath $submit -File -ErrorAction SilentlyContinue |
        Sort-Object Name |
        ForEach-Object {
            $hash = $null
            try { $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant() } catch { }
            [pscustomobject]@{
                Name = $_.Name
                Length = $_.Length
                LastWriteTimeUtc = $_.LastWriteTimeUtc
                Sha256 = $hash
            }
        }
)
$after = @(
    Get-CimInstance Win32_Process |
        Where-Object { $_.ProcessId -in $candidatePids -or $_.Name -eq 'tu_wddm_kmt_probe_arm64.exe' } |
        Select-Object ProcessId, ParentProcessId, Name, CommandLine, CreationDate
)
[pscustomobject]@{
    CleanedAt = (Get-Date).ToString('o')
    ExpectedTrace = $expectedTrace
    Before = $before
    Actions = $actions
    After = $after
    Files = $files
} | ConvertTo-Json -Depth 8
