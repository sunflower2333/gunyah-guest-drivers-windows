$ErrorActionPreference = 'Continue'
# Enable interposer logging only for this run.
New-Item -ItemType File -Path 'C:\DroidVM\ZinkD3D\interpose.on' -Force | Out-Null
Remove-Item 'C:\DroidVM\ZinkD3D\interpose.log' -Force -ErrorAction SilentlyContinue
try {
    powershell -NoProfile -ExecutionPolicy Bypass -File C:\DroidVM\ZinkD3D\probe-ldr.ps1 | Select-String 'hr='
} finally {
    Remove-Item 'C:\DroidVM\ZinkD3D\interpose.on' -Force -ErrorAction SilentlyContinue
}
Write-Output ("log_bytes=" + (Get-Item 'C:\DroidVM\ZinkD3D\interpose.log' -ErrorAction SilentlyContinue).Length)
