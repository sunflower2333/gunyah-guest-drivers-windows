[CmdletBinding()]
param(
    [string]$ProbePath = 'C:\Users\Administrator\tu_wddm_kmt_probe_arm64.exe',
    [string]$CaptureScript = 'C:\Users\Administrator\capture-viogpu-kmt-etl.ps1',
    [string]$OutputDirectory = 'C:\Windows\Temp\viogpu-kmt-58062',
    [int]$TimeoutMilliseconds = 15000
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path -LiteralPath $ProbePath -PathType Leaf)) {
    throw "KMT probe does not exist: $ProbePath"
}
if (-not (Test-Path -LiteralPath $CaptureScript -PathType Leaf)) {
    throw "ETL capture script does not exist: $CaptureScript"
}

$devices = @(
    Get-PnpDevice -PresentOnly |
        Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' }
)
if ($devices.Count -ne 1) {
    throw "Expected exactly one present virtio-gpu device, found $($devices.Count)."
}

$driverProperty = Get-PnpDeviceProperty -InstanceId $devices[0].InstanceId -KeyName 'DEVPKEY_Device_Driver'
if ($null -eq $driverProperty.PSObject.Properties['Data'] -or
    [string]::IsNullOrWhiteSpace([string]$driverProperty.Data)) {
    throw "Device '$($devices[0].InstanceId)' has no driver registry key."
}

$driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($driverProperty.Data)"
$driverKey = Get-Item -LiteralPath $driverKeyPath
$driverVersion = [string]$driverKey.GetValue('DriverVersion', '')
if ($driverVersion -ne '100.6.101.58062') {
    throw "Expected active driver version 100.6.101.58062, got '$driverVersion'."
}

foreach ($name in @($driverKey.GetValueNames() | Where-Object { $_ -like 'NativeContextCreate*' })) {
    Remove-ItemProperty -LiteralPath $driverKeyPath -Name $name -Force
}

$captureText = & $CaptureScript `
    -OutputDirectory $OutputDirectory `
    -ProbePath $ProbePath `
    -ProbeTimeoutMilliseconds $TimeoutMilliseconds | Out-String
$capture = $captureText | ConvertFrom-Json

$driverKey = Get-Item -LiteralPath $driverKeyPath
$diagnostics = @(
    foreach ($name in $driverKey.GetValueNames() | Where-Object { $_ -like 'NativeContextCreate*' } | Sort-Object) {
        [pscustomobject]@{
            Name = $name
            Kind = [string]$driverKey.GetValueKind($name)
            Value = $driverKey.GetValue(
                $name,
                $null,
                [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames
            )
        }
    }
)

$stdout = if (Test-Path -LiteralPath $capture.StdoutPath -PathType Leaf) {
    [IO.File]::ReadAllText([string]$capture.StdoutPath)
}
else {
    ''
}
$stderr = if (Test-Path -LiteralPath $capture.StderrPath -PathType Leaf) {
    [IO.File]::ReadAllText([string]$capture.StderrPath)
}
else {
    ''
}

$result = [pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    DriverVersion = $driverVersion
    DeviceStatus = [string]$devices[0].Status
    DeviceInstanceId = $devices[0].InstanceId
    ProbeExitCode = [int]$capture.ProbeExitCode
    EtlPath = [string]$capture.EtlPath
    EtlLength = [long]$capture.EtlLength
    StdoutPath = [string]$capture.StdoutPath
    StderrPath = [string]$capture.StderrPath
    Stdout = $stdout
    Stderr = $stderr
    Diagnostics = $diagnostics
}

$summaryPath = Join-Path $OutputDirectory 'summary.json'
$result | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
$result | ConvertTo-Json -Depth 7
exit $result.ProbeExitCode
