$ErrorActionPreference = 'Continue'
Get-ChildItem 'C:\ProgramData\Microsoft\Windows\WER\Temp' -File -Filter '*.txt' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 6 |
    ForEach-Object {
        Write-Output "===== $($_.FullName) ====="
        Get-Content -LiteralPath $_.FullName -Encoding Unicode -ErrorAction SilentlyContinue |
            Select-String -Pattern 'Bugcheck|Module|Fault|Bucket|viogpu|D1|Arg[0-9]|Failure|Driver' -Context 1,2
    }
Get-ChildItem 'C:\ProgramData\Microsoft\Windows\WER\Temp' -File -Filter '*.xml' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 8 |
    ForEach-Object {
        Write-Output "===== $($_.FullName) ====="
        Get-Content -LiteralPath $_.FullName -Raw -ErrorAction SilentlyContinue |
            Select-String -Pattern 'Bugcheck|Module|Fault|Bucket|viogpu|D1|Arg[0-9]|Failure|Driver' -Context 1,2
    }
