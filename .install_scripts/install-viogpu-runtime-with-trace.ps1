[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\Administrator\viogpuwddm-58020',
    [string]$CertificatePath = 'C:\Users\Administrator\DroidVM_Test.cer',
    [string]$OutputDirectory = 'C:\DroidVM\viogpu-58020\runtime-logs'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmssfff'
$traceName = "DroidVM-VioGpu-Install-$stamp"
$etlPath = Join-Path $OutputDirectory "viogpu-install-$stamp.etl"
$resultPath = Join-Path $OutputDirectory "viogpu-install-$stamp.json"
$traceStarted = $false
$installOutput = @()

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

    $installOutput = @(
        & 'C:\Users\Administrator\install-viogpu-runtime-package.ps1' `
            -PackageRoot $PackageRoot -CertificatePath $CertificatePath
    )
}
finally {
    if ($traceStarted) {
        & logman.exe stop $traceName -ets | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Error "logman stop failed with exit code $LASTEXITCODE."
        }
    }
}

$result = [pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    TraceName = $traceName
    EtlPath = $etlPath
    EtlLength = (Get-Item -LiteralPath $etlPath).Length
    InstallOutput = $installOutput
}
$result | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $resultPath -Encoding UTF8
$result | ConvertTo-Json -Depth 7
