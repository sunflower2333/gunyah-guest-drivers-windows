[CmdletBinding()]
param(
  [ValidateSet('Install','Verify','Rollback','Uninstall')][string]$GpuAction = 'Install',
  [string]$GpuJournalPath,
  [string]$GpuInstanceId
)
$ErrorActionPreference='Continue'
$base = $PSScriptRoot
Write-Host "DroidVM ARM64 driver installer" -ForegroundColor Cyan
# GPU APIs are one package and one entry. Rollback/removal are deliberately
# GPU-only; storage/network packages keep their existing install behavior.
$gpuArguments = @{Action=$GpuAction; PackageRoot=(Join-Path $base 'drivers/viogpu')}
if ($GpuJournalPath) { $gpuArguments.JournalPath=$GpuJournalPath }
if ($GpuInstanceId) { $gpuArguments.InstanceId=$GpuInstanceId }
if ($GpuAction -ne 'Install') {
  try { & (Join-Path $base 'viogpu-unified-install.ps1') @gpuArguments }
  catch { Write-Error -ErrorAction Continue $_; exit 1 }
  return
}
# NetKVM last: touching a live NIC during replacement can bugcheck.
$order = @('viogpu','pvmpower','viostor','vioscsi','vioinput','NetKVM')

function Get-Pkgs {
  $r=@(); $pub=$null
  foreach($l in (pnputil /enum-drivers)){
    if($l -match 'Published Name\s*:\s*(\S+)'){ $pub=$matches[1] }
    elseif(($l -match 'Original Name\s*:\s*(\S+)') -and $pub){ $r += [pscustomobject]@{Pub=$pub;Orig=$matches[1].ToLower()}; $pub=$null }
  }
  ,$r
}
foreach($d in $order){
  $inf = Get-ChildItem (Join-Path $base "drivers\$d") -Filter *.inf -ErrorAction SilentlyContinue | Select-Object -First 1
  if(-not $inf){ Write-Host "skip $d (no inf found)" -ForegroundColor Yellow; continue }
  $orig = $inf.Name.ToLower()
  Write-Host ""
  Write-Host ("== install " + $d + " : " + $inf.Name + " ==") -ForegroundColor Cyan
  if ($d -eq 'viogpu') {
    try { & (Join-Path $base 'viogpu-unified-install.ps1') @gpuArguments }
    catch { Write-Error -ErrorAction Continue $_; exit 1 }
    # Never enumerate/localize pnputil output or delete the rollback GPU package.
    continue
  }
  # pvmpower binds to a root-enumerated ROOT\PVMPOWER device; create the devnode
  # (idempotent, also cleans filter-era leftovers) before installing its package.
  if($d -eq 'pvmpower'){ & (Join-Path $base 'pvmpower-devnode.ps1') }
  $before = @(Get-Pkgs | Where-Object { $_.Orig -eq $orig } | ForEach-Object Pub)
  pnputil /add-driver $inf.FullName /install | Write-Host
  $after = @(Get-Pkgs | Where-Object { $_.Orig -eq $orig } | ForEach-Object Pub)
  $new = @($after | Where-Object { $_ -notin $before }) | Select-Object -First 1
  if(-not $new){ $new = $after | Select-Object -Last 1 }
  foreach($p in $after){ if($p -ne $new){ Write-Host ("  remove old " + $p); pnputil /delete-driver $p /uninstall | Out-Null } }
}
Write-Host ""
Write-Host "DONE. Reboot so the boot drivers load the new version." -ForegroundColor Green
