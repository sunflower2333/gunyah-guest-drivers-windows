$ErrorActionPreference = 'Continue'
$dir = 'C:\DroidVM\ZinkD3D\etw'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$etl = Join-Path $dir 'dxgk.etl'
$xml = Join-Path $dir 'dxgk.xml'
Remove-Item $etl,$xml -Force -ErrorAction SilentlyContinue
& logman stop dxgktrace -ets 2>&1 | Out-Null

& logman create trace dxgktrace -ets -o $etl -nb 64 128 -bs 1024 -p "{802EC45A-1E99-4B83-9920-87C98277BA9D}" 0xffffffffffffffff 0xff 2>&1 | Out-String | Write-Output
& logman update trace dxgktrace -ets -p "{DB6F6DDB-AC77-4E88-8253-819DF9BBF140}" 0xffffffffffffffff 0xff 2>&1 | Out-Null

powershell -NoProfile -ExecutionPolicy Bypass -File C:\DroidVM\ZinkD3D\probe-ldr.ps1 2>&1 | Select-String 'hr=' | Out-String | Write-Output

& logman stop dxgktrace -ets 2>&1 | Out-Null
Start-Sleep -Seconds 2
& tracerpt.exe $etl -o $xml -of XML -lr -y 2>&1 | Select-Object -Last 3 | Out-String | Write-Output
Write-Output ("ETL_SIZE=" + (Get-Item $etl -ErrorAction SilentlyContinue).Length + " XML_SIZE=" + (Get-Item $xml -ErrorAction SilentlyContinue).Length)
