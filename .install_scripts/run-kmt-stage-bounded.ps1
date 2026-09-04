[CmdletBinding()]
param(
    [string]$ProbePath = 'C:\Users\Administrator\tu_wddm_kmt_probe_stage_arm64.exe',
    [ValidateSet('enumeration', 'open', 'device', 'context', 'allocation')]
    [string]$Stage = 'enumeration',
    [string]$OutputDirectory = 'C:\Users\Administrator\viogpu-58169\kmt',
    [int]$TimeoutMilliseconds = 20000,
    [switch]$SubmitNop,
    [switch]$StressLifecycle
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path -LiteralPath $ProbePath -PathType Leaf)) {
    throw "KMT probe does not exist: $ProbePath"
}
if ($Stage -ne 'allocation' -and ($SubmitNop -or $StressLifecycle)) {
    throw 'Submit and stress probes require the allocation stage.'
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmssfff'
$stdoutPath = Join-Path $OutputDirectory "$Stage-$stamp.stdout.txt"
$stderrPath = Join-Path $OutputDirectory "$Stage-$stamp.stderr.txt"
$arguments = @("--stage=$Stage")
if ($SubmitNop) {
    $arguments += '--submit-nop'
}
if ($StressLifecycle) {
    $arguments += '--stress-lifecycle'
}

$process = Start-Process -FilePath $ProbePath -ArgumentList $arguments -PassThru `
    -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
$timedOut = -not $process.WaitForExit($TimeoutMilliseconds)
if ($timedOut) {
    & taskkill.exe /PID $process.Id /T /F *> $null
    if (-not $process.WaitForExit(5000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    $exitCode = 124
} else {
    # Redirected streams can outlive the process handle notification. Wait
    # once without a timeout and refresh before reading the native exit code.
    $process.WaitForExit()
    $process.Refresh()
    $exitCode = [int]$process.ExitCode
}

$stdout = if (Test-Path -LiteralPath $stdoutPath -PathType Leaf) {
    [IO.File]::ReadAllText($stdoutPath)
} else { '' }
$stderr = if (Test-Path -LiteralPath $stderrPath -PathType Leaf) {
    [IO.File]::ReadAllText($stderrPath)
} else { '' }

[pscustomobject]@{
    Stage = $Stage
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
