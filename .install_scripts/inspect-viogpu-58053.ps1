$ErrorActionPreference = 'Continue'
Get-PnpDevice -PresentOnly:$false |
    Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' } |
    Select-Object Status,Problem,ProblemCode,Class,FriendlyName,InstanceId |
    Format-List
Get-CimInstance Win32_PnPSignedDriver |
    Where-Object { $_.DeviceID -like 'PCI\VEN_1AF4&DEV_1050*' } |
    Select-Object DeviceID,DriverVersion,InfName,DriverDate,Manufacturer,IsSigned |
    Format-List
Get-Service VioGpuWddm -ErrorAction SilentlyContinue |
    Select-Object Status,StartType,Name,PathName |
    Format-List
& pnputil.exe /enum-devices /instanceid 'PCI\VEN_1AF4&DEV_1050&SUBSYS_10501AF4&REV_01\3&11583659&1&28' /drivers
