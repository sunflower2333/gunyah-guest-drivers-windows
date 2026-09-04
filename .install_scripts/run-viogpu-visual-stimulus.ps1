$ErrorActionPreference = 'Continue'
Set-StrictMode -Version Latest

$root = 'C:\DroidVM\viogpu-58043'
$stdoutPath = Join-Path $root 'viogpu-visual-stimulus-stdout.log'
$stderrPath = Join-Path $root 'viogpu-visual-stimulus-stderr.log'
$exitPath = Join-Path $root 'viogpu-visual-stimulus-exit.txt'
$statePath = Join-Path $root 'viogpu-visual-stimulus-state.json'

Remove-Item -LiteralPath $stdoutPath, $stderrPath, $exitPath, $statePath -Force -ErrorAction SilentlyContinue
& (Join-Path $root 'viogpu-visual-stimulus-fullscreen.ps1') `
    -DurationSeconds 300 `
    -StatePath $statePath `
    1> $stdoutPath `
    2> $stderrPath
$exitCode = if ($?) { 0 } else { 1 }
$exitCode | Set-Content -LiteralPath $exitPath -Encoding ASCII
