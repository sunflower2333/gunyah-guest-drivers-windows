param(
    [Parameter(Mandatory)][string]$InstalledPayload,
    [ValidateSet('arm64','x64','x86')][string]$Architecture,
    [ValidateRange(1,32)][int]$Iterations = 4,
    [ValidateRange(1,300)][int]$TimeoutSeconds = 120,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$Architecture) { throw 'Explicit probe architecture required' }
$payload = (Resolve-Path $InstalledPayload).Path
$out = [IO.Path]::GetFullPath($OutputDirectory)
if ($out.StartsWith($payload, [StringComparison]::OrdinalIgnoreCase)) { throw 'Use an ordinary app directory outside installed payload' }
if (Test-Path $out) { throw 'Output directory must be new' }
# No loader, runtime, compiler or Vulkan overrides. Fail rather than silently
# turning an inherited diagnostic override into a misleading global pass.
foreach ($name in @('VK_DRIVER_FILES','VK_ICD_FILENAMES','OCL_ICD_FILENAMES','OCL_ICD_VENDORS','CLVK_CLSPV_PATH','CLVK_CONFIG_FILE')) {
    if ([Environment]::GetEnvironmentVariable($name, 'Process')) { throw "Remove inherited override before normal-loader testing: $name" }
}
New-Item -ItemType Directory $out | Out-Null
$binding = Get-Content "$payload/package-binding.json" -Raw | ConvertFrom-Json
foreach ($file in Get-ChildItem "$payload/probes/$Architecture" -File) {
    if ($file.Name -match '^(OpenCL|vulkan-1|viogpucl)\.dll$') { throw 'App-local runtime forbidden in normal loader probe' }
    $relative = "probes/$Architecture/$($file.Name)"
    if ((Get-FileHash $file.FullName).Hash -ne $binding.files_after_signing.$relative) { throw "Probe input hash mismatch: $relative" }
    Copy-Item $file.FullName $out
}
foreach ($iteration in 1..$Iterations) {
    $stdout = Join-Path $out "$iteration.stdout.txt"
    $stderr = Join-Path $out "$iteration.stderr.txt"
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process (Join-Path $out 'viogpu-opencl-check.exe') -WorkingDirectory $out -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $null = $process.Handle
    if (!$process.WaitForExit($TimeoutSeconds * 1000)) {
        & taskkill.exe /PID $process.Id /T /F
        throw "OpenCL iteration $iteration timed out; stop device round"
    }
    $process.Refresh(); $watch.Stop()
    [pscustomobject]@{Architecture=$Architecture; Iteration=$iteration; ExitCode=$process.ExitCode; ElapsedMilliseconds=$watch.ElapsedMilliseconds} |
        ConvertTo-Json | Set-Content (Join-Path $out "$iteration.result.json")
    Get-Content $stdout
    Get-Content $stderr
    if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0 -or
        !(Select-String $stdout -SimpleMatch 'PASS GPU kernel + copy + readback + events + compiler error propagation')) { throw "OpenCL iteration $iteration failed; stop device round" }
    if (!(Select-String $stdout -Pattern 'module OpenCL.dll=.+\\(System32|SysWOW64)\\OpenCL.dll')) { throw 'Probe did not use a system OpenCL loader' }
}
Write-Output "PASS $Architecture ordinary app system-loader GPU probes iterations=$Iterations; correlate host submissions and desktop health separately"
