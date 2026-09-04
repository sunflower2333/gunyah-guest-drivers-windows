$ErrorActionPreference = 'Stop'
Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 1001 } -MaxEvents 5 |
    ForEach-Object {
        [pscustomobject]@{
            TimeCreated = $_.TimeCreated
            Xml = $_.ToXml()
            Message = $_.Message
        }
    } | ConvertTo-Json -Depth 6
