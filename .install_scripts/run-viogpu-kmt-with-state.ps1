[CmdletBinding()]
param(
    [string]$ProbePath = "$env:USERPROFILE\tu_wddm_kmt_probe_arm64.exe",
    [string]$StateScript = "$env:USERPROFILE\inspect-viogpu-native-registry.ps1"
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

& $ProbePath
$probeExitCode = $LASTEXITCODE
Write-Output "ProbeExitCode=$probeExitCode"
if (Test-Path -LiteralPath $StateScript -PathType Leaf) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $StateScript
}
exit $probeExitCode
