$ErrorActionPreference = 'Continue'
Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=(Get-Date).AddMinutes(-30)} -ErrorAction SilentlyContinue |
    Where-Object { $_.Level -le 3 -or $_.ProviderName -match 'Service Control Manager|Kernel-PnP|CodeIntegrity|Display|Dxg' } |
    Select-Object TimeCreated,Id,LevelDisplayName,ProviderName,Message |
    Format-List
