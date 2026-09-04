[CmdletBinding()]
param(
    [string]$OutputDirectory = 'C:\Windows\Temp\viogpu-kmt-etl',
    [string]$ProbePath = "$env:USERPROFILE\tu_wddm_kmt_probe_arm64.exe",
    [int]$ProbeTimeoutMilliseconds = 15000
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path -LiteralPath $ProbePath -PathType Leaf)) {
    throw "KMT probe does not exist: $ProbePath"
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Get-ChildItem -LiteralPath $OutputDirectory -Force -ErrorAction SilentlyContinue |
    Remove-Item -Force

$stamp = Get-Date -Format 'yyyyMMdd-HHmmssfff'
$traceName = "DroidVM-VioGpu-Kmt-$stamp"
$etlPath = Join-Path $OutputDirectory "viogpu-kmt-$stamp.etl"
$stdoutPath = Join-Path $OutputDirectory "probe-$stamp.stdout.txt"
$stderrPath = Join-Path $OutputDirectory "probe-$stamp.stderr.txt"
$traceStarted = $false
$probeExitCode = $null

try {
    # Keep writing a bounded circular ETL and flush once per second. A native-context
    # crash can reboot the guest before the finally block runs, so the default
    # stop-time-only flush leaves an empty file and destroys the useful pre-crash trace.
    & logman.exe create trace $traceName -ow -o $etlPath `
        -f bincirc -max 64 -bs 64 -nb 4 32 -ft 00:00:01 `
        -p Microsoft-Windows-DxgKrnl 0xFFFFFFFFFFFFFFFF 0xFF -ets | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "logman create failed with exit code $LASTEXITCODE."
    }
    $traceStarted = $true

    & logman.exe update $traceName -p '{D6B96B2C-72BF-4CA5-BB89-9FCA5C82F020}' `
        0x7FFFFFFF 0xFF -ets | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "logman provider update failed with exit code $LASTEXITCODE."
    }

    Start-Sleep -Seconds 2

    $probe = Start-Process -FilePath $ProbePath -PassThru `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    if (-not $probe.WaitForExit($ProbeTimeoutMilliseconds)) {
        Stop-Process -Id $probe.Id -Force -ErrorAction SilentlyContinue
        $probeExitCode = 124
    }
    else {
        $probeExitCode = $probe.ExitCode
    }
}
finally {
    if ($traceStarted) {
        & logman.exe stop $traceName -ets | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Error "logman stop failed with exit code $LASTEXITCODE."
        }
    }
}

[pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    ProbePath = $ProbePath
    ProbeExitCode = $probeExitCode
    EtlPath = $etlPath
    EtlLength = (Get-Item -LiteralPath $etlPath).Length
    StdoutPath = $stdoutPath
    StderrPath = $stderrPath
} | ConvertTo-Json
