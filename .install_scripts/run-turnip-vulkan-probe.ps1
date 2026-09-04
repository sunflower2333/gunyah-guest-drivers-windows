[CmdletBinding()]
param(
    [string]$BundleRoot = 'C:\DroidVM\TurnipRuns\mesa-run-33020538775',
    [string]$Probe = 'tu_wddm_vulkan_probe_arm64.exe',
    [string[]]$ProbeArgs = @(),
    [string]$LoaderDebug = '',
    [switch]$WddmDiagnostics
)

$ErrorActionPreference = 'Continue'

$env:VK_ICD_FILENAMES = Join-Path $BundleRoot 'freedreno_icd.arm64.json'
$env:VK_DRIVER_FILES = $env:VK_ICD_FILENAMES
if ($LoaderDebug -ne '') { $env:VK_LOADER_DEBUG = $LoaderDebug }
if ($WddmDiagnostics) { $env:TU_WDDM_DIAGNOSTICS = '1' } else { Remove-Item Env:TU_WDDM_DIAGNOSTICS -ErrorAction SilentlyContinue }

Write-Output "BUNDLE=$BundleRoot"
Write-Output "PROBE=$Probe"
Write-Output "VK_ICD_FILENAMES=$($env:VK_ICD_FILENAMES)"
Write-Output "ICD_EXISTS=$(Test-Path -LiteralPath $env:VK_ICD_FILENAMES)"
Write-Output "DLL_EXISTS=$(Test-Path -LiteralPath (Join-Path $BundleRoot 'vulkan_freedreno.dll'))"
Write-Output '--- probe output ---'

Push-Location $BundleRoot
try {
    $exe = Join-Path $BundleRoot $Probe
    & $exe @ProbeArgs 2>&1 | ForEach-Object { $_.ToString() }
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
Write-Output '--- end ---'
Write-Output "EXITCODE=$code"
