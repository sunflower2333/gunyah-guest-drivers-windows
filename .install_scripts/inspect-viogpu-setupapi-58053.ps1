$ErrorActionPreference = 'Continue'
$path = 'C:\Windows\INF\setupapi.dev.log'
Get-Content -LiteralPath $path -Tail 1200 |
    Select-String -Pattern 'viogpuwddm|PCI\\VEN_1AF4&DEV_1050|0xC0000490|!!!|error' -Context 2,4 |
    Select-Object -Last 80 |
    Out-String -Width 240
