$ErrorActionPreference = 'Continue'
Get-WinEvent -ListLog '*DxgKrnl*' -ErrorAction SilentlyContinue |
    Select-Object LogName,IsEnabled,RecordCount,LogMode,MaximumSizeInBytes |
    Format-List
foreach ($log in @(Get-WinEvent -ListLog '*DxgKrnl*' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty LogName)) {
    Write-Output "===== $log ====="
    Get-WinEvent -LogName $log -MaxEvents 200 -ErrorAction SilentlyContinue |
        Select-Object TimeCreated,Id,LevelDisplayName,ProviderName,Message |
        Format-List
}
