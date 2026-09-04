$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$outputDirectory = 'C:\Users\Administrator\kmt-fresh-pid'
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$stdoutPath = Join-Path $outputDirectory 'probe.stdout.txt'
$stderrPath = Join-Path $outputDirectory 'probe.stderr.txt'
Remove-Item -LiteralPath $stdoutPath,$stderrPath -Force -ErrorAction SilentlyContinue
$probe = Start-Process -FilePath 'C:\Users\Administrator\tu_wddm_kmt_probe_arm64.exe' -WorkingDirectory 'C:\Users\Administrator' -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru
[pscustomobject]@{ ProcessId = $probe.Id; StartedAt = (Get-Date).ToString('o'); OutputDirectory = $outputDirectory } | ConvertTo-Json
