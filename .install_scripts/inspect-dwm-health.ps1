$ErrorActionPreference='Continue'
Write-Output "=== DWM PROCESSES ==="
Get-Process dwm -ErrorAction SilentlyContinue | ForEach-Object {
    "pid={0} session={1} cpu={2} started={3}" -f $_.Id, $_.SessionId, [math]::Round($_.CPU,1), $_.StartTime
}
Write-Output ""
Write-Output "=== DWM APPLICATION EVENTS ==="
Get-WinEvent -FilterHashtable @{LogName='Application'} -MaxEvents 300 -ErrorAction SilentlyContinue |
  Where-Object { $_.ProviderName -like '*Desktop Window Manager*' } |
  Select-Object -First 5 | ForEach-Object { "{0} id={1} {2}" -f $_.TimeCreated, $_.Id, ($_.Message -split "`n")[0] }
Write-Output ""
Write-Output "=== SYSTEM ERRORS (display-ish, last 300) ==="
Get-WinEvent -FilterHashtable @{LogName='System'; Level=2} -MaxEvents 300 -ErrorAction SilentlyContinue |
  Where-Object { $_.ProviderName -match 'Display|Dxgk|VioGpu|Graphics' } |
  Select-Object -First 6 | ForEach-Object { "{0} [{1}] id={2} {3}" -f $_.TimeCreated, $_.ProviderName, $_.Id, ($_.Message -split "`n")[0] }
Write-Output ""
Write-Output "=== SESSION 1 STATE ==="
qwinsta 2>&1 | Out-String
