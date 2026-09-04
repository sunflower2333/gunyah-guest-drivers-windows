[CmdletBinding()]
param(
    [string]$Root = 'C:\DroidVM\ZinkD3D2',
    [string]$LogFile = 'C:\DroidVM\ZinkD3D2\mesa.log',
    [string]$MesaDebug = '',
    [string]$ZinkDebug = ''
)
$ErrorActionPreference = 'Continue'
Remove-Item -LiteralPath $LogFile -ErrorAction SilentlyContinue

$env:MESA_LOG_FILE = $LogFile
$env:GALLIUM_DRIVER = 'zink'
if ($MesaDebug -ne '') { $env:MESA_DEBUG = $MesaDebug }
if ($ZinkDebug -ne '') { $env:ZINK_DEBUG = $ZinkDebug }
$env:MESA_VK_ABORT_ON_DEVICE_LOSS = '1'

Push-Location $Root
try {
    $out = & .\zink_d3d11_offscreen.exe 2>&1 | ForEach-Object { $_.ToString() }
    $code = $LASTEXITCODE
} finally { Pop-Location }

Write-Output "--- probe stdout/stderr ---"
$out | ForEach-Object { $_ }
Write-Output "EXITCODE=$code"
Write-Output "--- mesa log ---"
if (Test-Path -LiteralPath $LogFile) {
    Get-Content -LiteralPath $LogFile -Tail 40
} else {
    Write-Output "<no mesa log written>"
}
