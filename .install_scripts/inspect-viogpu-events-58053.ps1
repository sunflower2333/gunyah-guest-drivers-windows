$ErrorActionPreference = 'Continue'
foreach ($log in @('System','Microsoft-Windows-DxgKrnl/Admin','Microsoft-Windows-Kernel-PnP/Configuration')) {
    Write-Output "===== $log ====="
    Get-WinEvent -FilterHashtable @{LogName=$log; StartTime=(Get-Date).AddMinutes(-30)} -ErrorAction SilentlyContinue |
        Where-Object { $_.ProviderName -match 'VioGpu|DxgKrnl|Kernel-PnP|Service Control Manager' -or $_.Message -match '1050|VioGpu|viogpu|c0000490|PnP' } |
        Select-Object TimeCreated,Id,LevelDisplayName,ProviderName,Message |
        Format-List
}
