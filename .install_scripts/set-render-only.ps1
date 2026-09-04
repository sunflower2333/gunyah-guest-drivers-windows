[CmdletBinding()]
param(
    [ValidateSet(0,1)][int]$Value = 0
)
$ErrorActionPreference = 'Stop'
$svc = 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
$par = "$svc\Parameters"

if (-not (Test-Path -LiteralPath $par)) { New-Item -Path $par -Force | Out-Null }
$before = (Get-Item -LiteralPath $par).GetValue('RenderOnly', '<unset>')
Write-Output "RenderOnly_before=$before"

Set-ItemProperty -LiteralPath $par -Name 'RenderOnly' -Value $Value -Type DWord
$after = (Get-Item -LiteralPath $par).GetValue('RenderOnly', '<unset>')
Write-Output "RenderOnly_after=$after"

# The driver reads this in DriverEntry, so it only takes effect on driver load.
Write-Output "NOTE=requires guest reboot for DriverEntry to re-read"
