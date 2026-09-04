$ErrorActionPreference = 'Continue'
$instance = 'PCI\VEN_1AF4&DEV_1050&SUBSYS_10501AF4&REV_01\3&11583659&1&28'
wevtutil.exe cl Microsoft-Windows-DxgKrnl-Admin
& pnputil.exe /remove-device $instance
Start-Sleep -Seconds 3
& pnputil.exe /scan-devices
Start-Sleep -Seconds 8
& pnputil.exe /enum-devices /instanceid $instance /drivers
