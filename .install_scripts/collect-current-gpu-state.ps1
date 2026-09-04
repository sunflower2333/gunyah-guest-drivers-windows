$ErrorActionPreference = 'Continue'
Set-StrictMode -Version Latest

$device = @(Get-PnpDevice | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
$service = Get-Service -Name 'VioGpuWddm' -ErrorAction SilentlyContinue
$rows = @()
foreach ($d in $device) {
    $driver = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Driver' -ErrorAction SilentlyContinue
    $key = $null
    if ($null -ne $driver -and -not [string]::IsNullOrWhiteSpace([string]$driver.Data)) {
        $key = Get-Item -LiteralPath ("Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\{0}" -f $driver.Data) -ErrorAction SilentlyContinue
    }
    $values = [ordered]@{}
    if ($null -ne $key) {
        foreach ($name in ($key.GetValueNames() | Where-Object { $_ -like 'Native*' } | Sort-Object)) {
            $values[$name] = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        }
    }
    $rows += [pscustomobject]@{
        Status = [string]$d.Status
        ProblemCode = if ($null -ne $d.PSObject.Properties['ProblemCode']) { $d.ProblemCode } else { $null }
        InstanceId = [string]$d.InstanceId
        DriverKey = if ($null -ne $driver) { [string]$driver.Data } else { $null }
        NativeValues = $values
    }
}

[pscustomobject]@{
    Time = (Get-Date).ToString('o')
    Service = if ($null -ne $service) { [pscustomobject]@{ State = $service.Status; Name = $service.Name; DisplayName = $service.DisplayName } } else { $null }
    Devices = $rows
    Events = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = (Get-Date).AddHours(-6) } -ErrorAction SilentlyContinue |
        Where-Object { $_.ProviderName -match 'VioGpu|DxgKrnl|Kernel-PnP|Service Control Manager' -or $_.Message -match '1050|VioGpu|viogpu|PnP|device' } |
        Select-Object -First 80 TimeCreated, Id, LevelDisplayName, ProviderName, Message)
} | ConvertTo-Json -Depth 8
