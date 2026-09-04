$ErrorActionPreference = 'Continue'
Set-StrictMode -Version Latest

$root = 'C:\Users\USER\viogpu-58173-c5c742f6'
$submit = Join-Path $root 'kmt-submit-nop'
$driverPath = Join-Path $env:SystemRoot 'System32\drivers\viogpuwddm.sys'
$device = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
})
$processRows = @(
    Get-CimInstance Win32_Process -Filter "Name='logman.exe' OR Name='tu_wddm_kmt_probe_arm64.exe'" |
        Select-Object ProcessId, ParentProcessId, Name, CommandLine, CreationDate
)
$files = @(
    Get-ChildItem -LiteralPath $submit -File -ErrorAction SilentlyContinue |
        Sort-Object Name |
        ForEach-Object {
            [pscustomobject]@{
                Name = $_.Name
                Length = $_.Length
                LastWriteTimeUtc = $_.LastWriteTimeUtc
                Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        }
)
$registry = [ordered]@{}
if ($device.Count -eq 1) {
    $property = Get-PnpDeviceProperty -InstanceId $device[0].InstanceId -KeyName 'DEVPKEY_Device_Driver'
    $keyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($property.Data)"
    $key = Get-Item -LiteralPath $keyPath -ErrorAction SilentlyContinue
    if ($null -ne $key) {
        foreach ($name in @($key.GetValueNames() | Where-Object {
            $_.StartsWith('NativeContext', [StringComparison]::Ordinal)
        } | Sort-Object)) {
            $registry[$name] = $key.GetValue(
                $name,
                $null,
                [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames
            )
        }
    }
}
[pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    RootExists = Test-Path -LiteralPath $root -PathType Container
    SubmitDirectory = $submit
    Files = $files
    Processes = $processRows
    Devices = @($device | Select-Object Status, ProblemCode, InstanceId, FriendlyName)
    DriverPath = $driverPath
    DriverExists = Test-Path -LiteralPath $driverPath -PathType Leaf
    DriverSha256 = if (Test-Path -LiteralPath $driverPath -PathType Leaf) {
        (Get-FileHash -LiteralPath $driverPath -Algorithm SHA256).Hash.ToLowerInvariant()
    } else { $null }
    NativeContext = $registry
} | ConvertTo-Json -Depth 10
