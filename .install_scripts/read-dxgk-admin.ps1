$ErrorActionPreference='Continue'
Get-WinEvent -LogName 'Microsoft-Windows-DxgKrnl-Admin' -MaxEvents 20 -ErrorAction SilentlyContinue |
  ForEach-Object {
    "--- {0} id={1} {2} ---" -f $_.TimeCreated, $_.Id, $_.LevelDisplayName
    $_.Message
  }
