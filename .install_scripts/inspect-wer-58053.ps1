$ErrorActionPreference = 'Continue'
Get-ChildItem 'C:\ProgramData\Microsoft\Windows\WER','C:\Windows\LiveKernelReports','C:\Windows\Minidump' -Recurse -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 30 FullName,Length,LastWriteTime | Format-Table -AutoSize
Get-ChildItem 'C:\ProgramData\Microsoft\Windows\WER' -Recurse -File -Filter '*.wer' -ErrorAction SilentlyContinue |
    ForEach-Object { Select-String -LiteralPath $_.FullName -Pattern 'Faulting|Bucket|Module|Bugcheck|0x000000D1|viogpu' -SimpleMatch -ErrorAction SilentlyContinue }
