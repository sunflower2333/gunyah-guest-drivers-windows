[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$devices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
if ($devices.Count -ne 1) { throw "Expected one present virtio-gpu device, found $($devices.Count)." }
$property = Get-PnpDeviceProperty -InstanceId $devices[0].InstanceId -KeyName 'DEVPKEY_Device_Driver'
$driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($property.Data)"
$key = Get-Item -LiteralPath $driverKeyPath

Write-Output "DRIVER_KEY=$driverKeyPath"
Write-Output "DEVICE_STATUS=$($devices[0].Status) PROBLEM=$($devices[0].Problem)"
$names = @($key.GetValueNames() | Where-Object { $_ -like 'Native*' -or $_ -like 'Viogpu*' -or $_ -like 'Wddm*' })
if ($names.Count -eq 0) {
    Write-Output 'NATIVE_DIAGNOSTICS=<none>'
} else {
    foreach ($n in ($names | Sort-Object)) {
        $v = $key.GetValue($n)
        if ($v -is [int] -or $v -is [uint32] -or $v -is [long]) {
            Write-Output ("{0} = {1} (0x{1:X8})" -f $n, $v)
        } else {
            Write-Output ("{0} = {1}" -f $n, ($v -join ','))
        }
    }
}
