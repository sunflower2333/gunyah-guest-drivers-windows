$ErrorActionPreference = 'Continue'
wevtutil.exe cl Microsoft-Windows-DxgKrnl-Admin
wevtutil.exe cl Microsoft-Windows-DxgKrnl-Operational
& pnputil.exe /scan-devices
Start-Sleep -Seconds 5
& pnputil.exe /enum-devices /instanceid 'PCI\VEN_1AF4&DEV_1050&SUBSYS_10501AF4&REV_01\3&11583659&1&28' /drivers
