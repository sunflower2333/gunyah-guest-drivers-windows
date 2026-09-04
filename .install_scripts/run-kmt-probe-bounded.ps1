[CmdletBinding()]
param(
    [string]$ProbePath = 'C:\Users\Administrator\tu_wddm_kmt_probe_arm64.exe',
    [string]$OutputDirectory = 'C:\Users\Administrator\viogpu-32753307099\kmt-direct',
    [int]$TimeoutMilliseconds = 20000
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path -LiteralPath $ProbePath -PathType Leaf)) {
    throw "KMT probe does not exist: $ProbePath"
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmssfff'
$stdoutPath = Join-Path $OutputDirectory "probe-$stamp.stdout.txt"
$stderrPath = Join-Path $OutputDirectory "probe-$stamp.stderr.txt"
$process = Start-Process -FilePath $ProbePath -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
$timedOut = -not $process.WaitForExit($TimeoutMilliseconds)
if ($timedOut) {
    # KMT may leave a descendant in a system thunk after the wrapper times out.
    # Terminate the whole process tree so the next probe cannot inherit stale state.
    & taskkill.exe /PID $process.Id /T /F *> $null
    if (-not $process.WaitForExit(5000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    $exitCode = 124
}
else {
    $exitCode = $process.ExitCode
}

$stdout = if (Test-Path -LiteralPath $stdoutPath -PathType Leaf) {
    [System.IO.File]::ReadAllText($stdoutPath)
}
else {
    ''
}
$stderr = if (Test-Path -LiteralPath $stderrPath -PathType Leaf) {
    [System.IO.File]::ReadAllText($stderrPath)
}
else {
    ''
}

[pscustomobject]@{
    ProbePath = $ProbePath
    ProcessId = $process.Id
    TimedOut = $timedOut
    ExitCode = $exitCode
    StdoutPath = $stdoutPath
    StderrPath = $stderrPath
    Stdout = $stdout
    Stderr = $stderr
} | ConvertTo-Json -Depth 5

exit $exitCode
