$ErrorActionPreference = 'Continue'
$exe = 'C:\Users\Administrator\tu_wddm_kmt_probe_arm64.exe'
$out = 'C:\Users\Administrator\tu_wddm_kmt_probe-60s.stdout.txt'
$err = 'C:\Users\Administrator\tu_wddm_kmt_probe-60s.stderr.txt'
Remove-Item -LiteralPath $out,$err -Force -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
if (-not $p.WaitForExit(60000)) {
  Write-Output "TIMEOUT pid=$($p.Id)"
  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
  $p.WaitForExit()
} else {
  Write-Output "EXIT code=$($p.ExitCode)"
}
Write-Output '=== STDOUT ==='
Get-Content -LiteralPath $out -ErrorAction SilentlyContinue
Write-Output '=== STDERR ==='
Get-Content -LiteralPath $err -ErrorAction SilentlyContinue
