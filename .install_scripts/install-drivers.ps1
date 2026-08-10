$ErrorActionPreference='Stop'
$base = $PSScriptRoot
Write-Host "DroidVM ARM64 driver installer" -ForegroundColor Cyan
# rdmapool first because the pVM virtio drivers depend on its restricted DMA interface.
# NetKVM last: touching a live NIC during replacement can bugcheck.
$order = @('rdmapool','viogpu','pvmpower','viostor','vioscsi','vioinput','NetKVM')

function Invoke-PnpUtil([string[]]$Arguments) {
  $output = & pnputil @Arguments 2>&1
  $exitCode = $LASTEXITCODE
  $output | Write-Host
  if($exitCode -ne 0){ throw "pnputil failed with exit code $exitCode`: pnputil $($Arguments -join ' ')" }
}

$certPath = Join-Path $base 'DroidVM_Test.cer'
if(-not (Test-Path $certPath)){ throw "missing signing certificate: $certPath" }
$artifactCert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($certPath)
foreach($store in @('Cert:\LocalMachine\Root','Cert:\LocalMachine\TrustedPublisher')){
  if(-not (Test-Path (Join-Path $store $artifactCert.Thumbprint))){
    Import-Certificate -FilePath $certPath -CertStoreLocation $store | Out-Null
    Write-Host "trusted artifact certificate in $store`: $($artifactCert.Thumbprint)"
  }
}

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
  # pvmpower binds to a root-enumerated ROOT\PVMPOWER device; create the devnode
  # (idempotent, also cleans filter-era leftovers) before installing its package.
  if($d -eq 'pvmpower'){ & (Join-Path $base 'pvmpower-devnode.ps1') }
  $before = @(Get-Pkgs | Where-Object { $_.Orig -eq $orig } | ForEach-Object Pub)
  Invoke-PnpUtil -Arguments @('/add-driver',$inf.FullName,'/install')
  $after = @(Get-Pkgs | Where-Object { $_.Orig -eq $orig } | ForEach-Object Pub)
  $new = @($after | Where-Object { $_ -notin $before }) | Select-Object -First 1
  if(-not $new){
    $new = $after | Select-Object -Last 1
    if(-not $new){ throw "pnputil reported success but no installed package was found for $orig" }
  }
  foreach($p in $after){
    if($p -ne $new){
      Write-Host ("  remove old " + $p)
      Invoke-PnpUtil -Arguments @('/delete-driver',$p,'/uninstall')
    }
  }
}
Write-Host ""
Write-Host "DONE. Reboot so the boot drivers (viostor/rdmapool) load the new version." -ForegroundColor Green
