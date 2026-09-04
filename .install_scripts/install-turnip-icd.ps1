[CmdletBinding()]
param(
    [string]$BundleRoot = 'C:\DroidVM\TurnipRuns\run-33322445949',
    [ValidateSet('Install','Uninstall')]
    [string]$Action = 'Install'
)

$ErrorActionPreference = 'Stop'
# The bundle installer declares ConfirmImpact High, which prompts and then
# throws in a non-interactive session.  Suppress confirmation for this scope.
$ConfirmPreference = 'None'

$script = Join-Path $BundleRoot 'turnip-wddm-icd.ps1'
if (-not (Test-Path -LiteralPath $script)) { throw "Missing ICD installer: $script" }

& $script -Action $Action -BundleRoot $BundleRoot -Confirm:$false

Write-Output "--- registry after $Action ---"
foreach ($k in @('HKLM:\SOFTWARE\Khronos\Vulkan\Drivers')) {
    if (Test-Path $k) {
        (Get-Item $k).GetValueNames() | ForEach-Object { "{0} = {1}" -f $_, (Get-ItemPropertyValue $k $_) }
    } else { Write-Output "$k : <absent>" }
}
