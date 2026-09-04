[CmdletBinding()]
param(
    [string]$OutputPath = 'C:\DroidVM\TurnipRuns\evidence\post-workload-stability-20260831.json',
    [int]$LookbackMinutes = 90
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$devices = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
})
if ($devices.Count -ne 1) {
    throw "Expected one present virtio-gpu device, found $($devices.Count)."
}

$device = $devices[0]
$driverProperty = Get-PnpDeviceProperty `
    -InstanceId $device.InstanceId `
    -KeyName 'DEVPKEY_Device_Driver'
$driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($driverProperty.Data)"
$driverKey = Get-Item -LiteralPath $driverKeyPath
$service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
$imagePath = [Environment]::ExpandEnvironmentVariables([string]$service.GetValue('ImagePath', '')).Trim('"')
if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
    $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
}

$nativeDiagnostics = [ordered]@{}
foreach ($name in @($driverKey.GetValueNames() | Where-Object {
    $_.StartsWith('NativeContext', [StringComparison]::Ordinal)
} | Sort-Object)) {
    $nativeDiagnostics[$name] = $driverKey.GetValue(
        $name,
        $null,
        [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames
    )
}

$since = (Get-Date).AddMinutes(-$LookbackMinutes)
$systemEvents = @(
    Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since } -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -in @(14, 17, 18, 19, 41, 1001, 4101, 6008) } |
        Select-Object -First 100 TimeCreated, Id, ProviderName, LevelDisplayName, Message
)
$applicationEvents = @(
    Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $since } -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Id -in @(1000, 1001) -or
            $_.ProviderName -match 'Desktop Window Manager|Application Error|Windows Error Reporting'
        } |
        Select-Object -First 100 TimeCreated, Id, ProviderName, LevelDisplayName, Message
)

$evidenceRoot = 'C:\DroidVM\TurnipRuns\evidence'
$workloadEvidence = @(
    Get-ChildItem -LiteralPath $evidenceRoot -File -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Name -like 'lifecycle-*' -or
            $_.Name -like 'compute-*' -or
            $_.Name -like 'graphics-*' -or
            $_.Name -like 'kmt-*-fresh-20260831*' -or
            $_.Name -like 'win32-interactive-*'
        } |
        Sort-Object Name |
        Select-Object Name, Length, LastWriteTime
)

$os = Get-CimInstance Win32_OperatingSystem
$result = [ordered]@{
    CollectedAt = (Get-Date).ToString('o')
    LastBootUpTime = ([datetime]$os.LastBootUpTime).ToString('o')
    Device = [ordered]@{
        Status = [string]$device.Status
        ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) {
            $device.ProblemCode
        } else { $null }
        InstanceId = [string]$device.InstanceId
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        DriverImagePath = $imagePath
        DriverImageSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $imagePath).Hash.ToLowerInvariant()
        NativeContextDiagnostics = $nativeDiagnostics
    }
    Processes = @(
        Get-Process dwm, explorer -ErrorAction SilentlyContinue |
            Select-Object Name, Id, SessionId, StartTime
    )
    SystemEvents = $systemEvents
    ApplicationEvents = $applicationEvents
    WorkloadEvidence = $workloadEvidence
    Dumps = @(
        Get-ChildItem -LiteralPath 'C:\Windows\Minidump' -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 10 FullName, Length, LastWriteTime
    )
}

$json = $result | ConvertTo-Json -Depth 10
[IO.File]::WriteAllText($OutputPath, $json)
$json
