[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Name,
    [string]$RelayBase = 'http://192.168.1.229:18080',
    [string]$OutDir = 'C:\DroidVM\ZinkD3D',
    [string]$ExpectedSha256 = ''
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$dest = Join-Path $OutDir $Name
Remove-Item -LiteralPath $dest -ErrorAction SilentlyContinue

$t = Get-Date
try {
    Invoke-WebRequest -Uri "$RelayBase/$Name" -OutFile $dest -UseBasicParsing -TimeoutSec 300
} catch {
    Write-Output "DOWNLOAD_FAILED: $($_.Exception.Message)"
    exit 1
}
$sec = [math]::Round(((Get-Date) - $t).TotalSeconds, 1)
$len = (Get-Item -LiteralPath $dest).Length
$sha = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash.ToLower()
Write-Output "bytes=$len seconds=$sec rate_kb_s=$([math]::Round($len/1KB/[math]::Max($sec,0.1),0))"
Write-Output "sha256=$sha"
if ($ExpectedSha256 -ne '') {
    if ($sha -eq $ExpectedSha256.ToLower()) { Write-Output "HASH_OK" } else { Write-Output "HASH_MISMATCH expected=$ExpectedSha256"; exit 1 }
}
