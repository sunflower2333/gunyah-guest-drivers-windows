$ErrorActionPreference = 'Stop'
$device = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
if ($device.Count -ne 1) { throw "Expected one virtio-gpu device, got $($device.Count)" }
$id = $device[0].InstanceId
"InstanceId=$id"
"Status=$($device[0].Status)"
"--- pnputil resources ---"
& pnputil.exe /enum-devices /instanceid $id /resources
"--- properties ---"
Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_ResourceRequirements,DEVPKEY_Device_AllocConfig,DEVPKEY_Device_BusNumber -ErrorAction SilentlyContinue |
    Format-List KeyName,Data
