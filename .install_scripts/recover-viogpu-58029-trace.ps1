$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$sessions = @(& logman.exe query -ets 2>&1)
$traceNames = @(
    $sessions |
        ForEach-Object { [string]$_ } |
        Where-Object { $_ -match '^DroidVM-VioGpu-' } |
        ForEach-Object { ($_ -split '\s+')[0] }
)
$stopped = @()
foreach ($traceName in $traceNames) {
    $output = @(& logman.exe stop $traceName -ets 2>&1)
    $stopped += [pscustomobject]@{
        Name = $traceName
        ExitCode = $LASTEXITCODE
        Output = $output
    }
}

$outputDirectory = 'C:\DroidVM\viogpu-58029\runtime-logs'
$files = @(
    Get-ChildItem -LiteralPath $outputDirectory -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc |
        Select-Object Name, Length, LastWriteTimeUtc
)
$processes = @(
    Get-Process -Name pnputil, logman, tracerpt -ErrorAction SilentlyContinue |
        Select-Object Id, ProcessName, StartTime, CPU, Responding
)

[pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    StoppedSessions = $stopped
    RuntimeFiles = $files
    RelevantProcesses = $processes
} | ConvertTo-Json -Depth 6
