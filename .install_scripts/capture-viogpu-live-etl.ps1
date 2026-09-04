[CmdletBinding()]
param(
    [string]$OutputDirectory = 'C:\Windows\Temp\viogpu-live-etl',
    [int]$DurationSeconds = 10
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Get-ChildItem -LiteralPath $OutputDirectory -Force -ErrorAction SilentlyContinue | Remove-Item -Force

$stamp = Get-Date -Format 'yyyyMMdd-HHmmssfff'
$traceName = "DroidVM-VioGpu-Live-$stamp"
$etlPath = Join-Path $OutputDirectory "viogpu-live-$stamp.etl"
$traceStarted = $false

try {
    & logman.exe create trace $traceName -ow -o $etlPath `
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

    Start-Process notepad.exe
    Start-Sleep -Seconds $DurationSeconds
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
    EtlPath = $etlPath
    EtlLength = (Get-Item -LiteralPath $etlPath).Length
} | ConvertTo-Json
