$ErrorActionPreference = 'Stop'
Get-ChildItem -LiteralPath 'C:\Users\Administrator' -Directory |
    Where-Object { $_.Name -match 'symbol|analysis|5806' } |
    Select-Object Name,FullName
Get-ChildItem -LiteralPath 'C:\Users\Administrator\viogpu-analysis-58066\symbols' -Recurse -Filter 'ntkrnlmp.pdb' -ErrorAction SilentlyContinue |
    Select-Object -First 5 FullName,Length
