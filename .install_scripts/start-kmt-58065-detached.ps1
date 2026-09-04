$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$outputDirectory = 'C:\Users\Administrator\kmt-32799091676'
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$launcherOutput = Join-Path $outputDirectory 'launcher.stdout.txt'
$launcherError = Join-Path $outputDirectory 'launcher.stderr.txt'
$probeOutput = Join-Path $outputDirectory 'probe.result.json'
$probeError = Join-Path $outputDirectory 'probe.result.err.txt'
foreach ($path in @($launcherOutput, $launcherError, $probeOutput, $probeError)) {
    Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
}
$arguments = @(
    '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
    '-File', 'C:\Users\Administrator\run-kmt-probe-bounded.ps1',
    '-ProbePath', 'C:\Users\Administrator\tu_wddm_kmt_probe_arm64.exe',
    '-OutputDirectory', $outputDirectory,
    '-TimeoutMilliseconds', '20000'
)
$process = Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -WindowStyle Hidden -RedirectStandardOutput $probeOutput -RedirectStandardError $probeError -PassThru
[pscustomobject]@{
    StartedAt = (Get-Date).ToString('o')
    ProcessId = $process.Id
    OutputDirectory = $outputDirectory
    ResultPath = $probeOutput
    ErrorPath = $probeError
} | ConvertTo-Json
