$ErrorActionPreference='Continue'
Write-Output "=== DXGK-RELATED LOGS AVAILABLE ==="
Get-WinEvent -ListLog * -ErrorAction SilentlyContinue |
  Where-Object { $_.LogName -match 'Dxgk|DirectX|Display|Kernel-Pnp' } |
  ForEach-Object { "{0} enabled={1} records={2}" -f $_.LogName, $_.IsEnabled, $_.RecordCount }

foreach ($log in @('Microsoft-Windows-DxgKrnl/Admin','Microsoft-Windows-DxgKrnl/Operational',
                   'Microsoft-Windows-Kernel-PnP/Configuration')) {
    Write-Output ""
    Write-Output "=== $log (last 8) ==="
    try {
        Get-WinEvent -LogName $log -MaxEvents 8 -ErrorAction Stop | ForEach-Object {
            "{0} id={1} {2} :: {3}" -f $_.TimeCreated, $_.Id, $_.LevelDisplayName, (($_.Message -split "`n")[0])
        }
    } catch { Write-Output "  <unavailable: $($_.Exception.Message)>" }
}

Write-Output ""
Write-Output "=== SYSTEM log, display/pnp errors+warnings (last 10) ==="
Get-WinEvent -FilterHashtable @{LogName='System'; Level=@(2,3)} -MaxEvents 400 -ErrorAction SilentlyContinue |
  Where-Object { $_.ProviderName -match 'Dxgk|Display|VioGpu|Pnp|Graphics' } |
  Select-Object -First 10 | ForEach-Object {
    "{0} [{1}] id={2} {3} :: {4}" -f $_.TimeCreated, $_.ProviderName, $_.Id, $_.LevelDisplayName, (($_.Message -split "`n")[0])
  }
