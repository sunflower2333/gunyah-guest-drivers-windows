[CmdletBinding()]
param(
    [string]$Root = 'C:\DroidVM\VulkanTools',
    [int[]]$FrameCounts = @(300, 900, 1800)
)
$ErrorActionPreference = 'Continue'
$exe = Join-Path $Root 'vkcube.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "missing $exe" }

Write-Output "exe=$exe"
Write-Output "sha256=$((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "session=$((Get-Process -Id $PID).SessionId)"

foreach ($n in $FrameCounts) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $out = & $exe --c $n 2>&1 | ForEach-Object { $_.ToString() }
    $code = $LASTEXITCODE
    $sw.Stop()
    $sec = $sw.Elapsed.TotalSeconds
    $fps = if ($sec -gt 0) { [math]::Round($n / $sec, 1) } else { 0 }
    "frames={0,-5} elapsed={1,7:N2}s fps={2,8} exit={3} output='{4}'" -f `
        $n, $sec, $fps, $code, ($out -join ' | ')
}
