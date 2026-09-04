$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Write-Output '=== KMT/probe files ==='
Get-ChildItem -LiteralPath 'C:\Users\Administrator' -File |
    Where-Object { $_.Name -match 'kmt|probe' } |
    Select-Object Name,Length,LastWriteTime

Write-Output '=== Present device ==='
$device = @(
    Get-PnpDevice -PresentOnly |
        Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' }
)
$device | Select-Object Status,Class,FriendlyName,InstanceId

if ($device.Count -eq 1) {
    Write-Output '=== Driver candidates ==='
    & pnputil.exe /enum-devices /instanceid $device[0].InstanceId /drivers
    if ($LASTEXITCODE -ne 0) {
        throw "pnputil device enumeration failed with exit code $LASTEXITCODE"
    }
}

Write-Output '=== Signed driver inventory ==='
Get-CimInstance Win32_PnPSignedDriver |
    Where-Object { $_.DeviceID -like 'PCI\VEN_1AF4&DEV_1050*' } |
    Select-Object DeviceID,DriverVersion,InfName,DriverDate,Manufacturer

Write-Output '=== Native registry ==='
if ($device.Count -eq 1) {
    $driverProperty = Get-PnpDeviceProperty -InstanceId $device[0].InstanceId -KeyName 'DEVPKEY_Device_Driver'
    $driverKey = [string]$driverProperty.Data
    $registryPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$driverKey"
    $key = Get-Item -LiteralPath $registryPath
    foreach ($name in $key.GetValueNames() | Where-Object { $_ -like 'Native*' } | Sort-Object) {
        [pscustomobject]@{ Name = $name; Kind = [string]$key.GetValueKind($name); Value = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) }
    }
}
