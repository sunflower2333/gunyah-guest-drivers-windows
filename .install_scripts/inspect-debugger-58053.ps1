$ErrorActionPreference = 'Continue'
Get-Command windbg,kd,cdb,ntsd -ErrorAction SilentlyContinue |
    Select-Object Name,Source,CommandType | Format-List
Get-ChildItem -Path 'C:\Program Files','C:\Program Files (x86)','C:\ProgramData' -Filter 'cdb.exe' -Recurse -ErrorAction SilentlyContinue |
    Select-Object FullName,Length | Format-List
