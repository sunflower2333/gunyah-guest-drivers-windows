$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Start-Process -FilePath "$env:SystemRoot\System32\shutdown.exe" `
    -ArgumentList @('/r', '/f', '/t', '0') `
    -WindowStyle Hidden

'Reboot request submitted.'
