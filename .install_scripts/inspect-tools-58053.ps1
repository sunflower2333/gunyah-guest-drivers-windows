$ErrorActionPreference = 'Continue'
Get-Command winget,dumpchk,werfault -ErrorAction SilentlyContinue |
    Select-Object Name,Source | Format-List
Get-ChildItem 'C:\Windows\System32' -Filter '*dump*' -ErrorAction SilentlyContinue |
    Select-Object Name,Length | Format-Table -AutoSize
