$ErrorActionPreference = 'Continue'
Set-StrictMode -Version Latest

$rows = @(
    Get-CimInstance Win32_Process |
        Where-Object {
            $_.Name -eq 'tu_wddm_kmt_probe_arm64.exe' -or
            ($_.Name -eq 'logman.exe' -and (
                [string]$_.CommandLine -like '*DroidVM-VioGpu-58173-SubmitNop-20260830-031954799*' -or
                [string]$_.CommandLine -match '(?i)logman\s+query\s+-ets'
            ))
        }
)
$result = @()
foreach ($row in $rows) {
    $processId = [int]$row.ProcessId
    $record = [ordered]@{
        ProcessId = $processId
        Name = [string]$row.Name
        CommandLine = [string]$row.CommandLine
        Before = $null
        KillResult = $null
        KillError = $null
        After = $null
    }
    try {
        $record.Before = Get-Process -Id $processId -ErrorAction Stop | Select-Object Id, HasExited, Responding, StartTime, CPU, Handles
    } catch { $record.Before = $_.Exception.Message }
    try {
        $process = Get-Process -Id $processId -ErrorAction Stop
        $process.Kill()
        $record.KillResult = 'Kill() returned'
    } catch { $record.KillError = $_.Exception.ToString() }
    Start-Sleep -Milliseconds 200
    try {
        $record.After = Get-Process -Id $processId -ErrorAction Stop | Select-Object Id, HasExited, Responding, StartTime, CPU, Handles
    } catch { $record.After = 'not found' }
    $result += [pscustomobject]$record
}
$result | ConvertTo-Json -Depth 8
