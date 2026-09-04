[CmdletBinding()]
param(
    [string]$ProbePath = 'C:\Users\Administrator\tu_wddm_kmt_probe_arm64.exe',
    [string]$OutputDirectory = 'C:\Users\Administrator\viogpu-58165\kmt',
    [string]$DeviceInstanceId = 'PCI\VEN_1AF4&DEV_1050&SUBSYS_10501AF4&REV_01\3&11583659&1&28',
    [string]$DriverKeyName = '{4d36e968-e325-11ce-bfc1-08002be10318}\0001',
    [int]$ProbeTimeoutMilliseconds = 20000
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedVersion = '100.6.101.58165'
$driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$DriverKeyName"
$driverKeyItem = Get-Item -LiteralPath $driverKeyPath
$driverVersion = [string]$driverKeyItem.GetValue('DriverVersion', '')
if ($driverVersion -ne $expectedVersion) {
    throw "Active driver version is '$driverVersion', expected '$expectedVersion'."
}
if ([string]$driverKeyItem.GetValue('MatchingDeviceId', '') -ne 'PCI\VEN_1AF4&DEV_1050') {
    throw "Driver key '$DriverKeyName' is not the virtio-gpu device key."
}
if (-not (Test-Path -LiteralPath $ProbePath -PathType Leaf)) {
    throw "KMT probe does not exist: $ProbePath"
}

$diagnosticPrefixes = @('NativeContextCreate', 'NativeContextGetParam')
function Test-DiagnosticName {
    param([string]$Name)

    foreach ($prefix in $diagnosticPrefixes) {
        if ($Name.StartsWith($prefix, [StringComparison]::Ordinal)) {
            return $true
        }
    }
    return $false
}

foreach ($name in @($driverKeyItem.GetValueNames())) {
    if (Test-DiagnosticName $name) {
        Remove-ItemProperty -LiteralPath $driverKeyPath -Name $name -ErrorAction Stop
    }
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Get-ChildItem -LiteralPath $OutputDirectory -Force -ErrorAction SilentlyContinue | Remove-Item -Force -Recurse

$stamp = Get-Date -Format 'yyyyMMdd-HHmmssfff'
$traceName = "DroidVM-VioGpu-GetParam-$stamp"
$etlPath = Join-Path $OutputDirectory "viogpu-getparam-$stamp.etl"
$stdoutPath = Join-Path $OutputDirectory "probe-$stamp.stdout.txt"
$stderrPath = Join-Path $OutputDirectory "probe-$stamp.stderr.txt"
$resultPath = Join-Path $OutputDirectory "result-$stamp.json"
$traceStarted = $false
$probeExitCode = $null
$probeProcessId = $null
$timedOut = $false

try {
    & logman.exe create trace $traceName -ow -o $etlPath -f bincirc -max 8 -bs 64 -nb 2 8 -ft 00:00:01 `
        -p Microsoft-Windows-DxgKrnl 0xFFFFFFFFFFFFFFFF 0xFF -ets | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "logman create failed with exit code $LASTEXITCODE."
    }
    $traceStarted = $true

    & logman.exe update $traceName -p '{D6B96B2C-72BF-4CA5-BB89-9FCA5C82F020}' `
        0x7FFFFFFF 0xFF -ets | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "logman provider update failed with exit code $LASTEXITCODE."
    }

    Start-Sleep -Seconds 2
    $probe = Start-Process -FilePath $ProbePath -WorkingDirectory (Split-Path $ProbePath) -PassThru `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    $probeProcessId = $probe.Id
    $timedOut = -not $probe.WaitForExit($ProbeTimeoutMilliseconds)
    if ($timedOut) {
        & taskkill.exe /PID $probe.Id /T /F *> $null
        if (-not $probe.WaitForExit(5000)) {
            Stop-Process -Id $probe.Id -Force -ErrorAction SilentlyContinue
        }
        $probeExitCode = 124
    }
    else {
        $probeExitCode = $probe.ExitCode
    }
}
finally {
    if ($traceStarted) {
        & logman.exe stop $traceName -ets | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "logman stop failed with exit code $LASTEXITCODE."
        }
    }
}

$driverKeyItem = Get-Item -LiteralPath $driverKeyPath
$diagnostic = [ordered]@{}
foreach ($name in @($driverKeyItem.GetValueNames() | Sort-Object)) {
    if (Test-DiagnosticName $name) {
        $diagnostic[$name] = $driverKeyItem.GetValue($name)
    }
}

$result = [pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    DriverVersion = $driverVersion
    DeviceInstanceId = $DeviceInstanceId
    ProbePath = $ProbePath
    ProbeHash = (Get-FileHash -LiteralPath $ProbePath -Algorithm SHA256).Hash.ToLowerInvariant()
    ProbeProcessId = $probeProcessId
    ProbeTimedOut = $timedOut
    ProbeExitCode = $probeExitCode
    EtlPath = $etlPath
    EtlLength = if (Test-Path -LiteralPath $etlPath -PathType Leaf) { (Get-Item -LiteralPath $etlPath).Length } else { 0 }
    StdoutPath = $stdoutPath
    StderrPath = $stderrPath
    Stdout = if (Test-Path -LiteralPath $stdoutPath -PathType Leaf) { [IO.File]::ReadAllText($stdoutPath) } else { '' }
    Stderr = if (Test-Path -LiteralPath $stderrPath -PathType Leaf) { [IO.File]::ReadAllText($stderrPath) } else { '' }
    Diagnostic = $diagnostic
}
$json = $result | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText($resultPath, $json)
$json

exit $probeExitCode
