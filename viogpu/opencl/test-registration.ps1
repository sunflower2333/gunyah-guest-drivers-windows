$ErrorActionPreference = 'Stop'
Import-Module "$PSScriptRoot/../../.install_scripts/opencl-registration.psm1" -Force
$fake = Join-Path $env:TEMP ('OpenCL registration test ' + [Guid]::NewGuid().ToString('N'))
$entries = @(Get-OpenClEntries $fake)
$before = @(Get-OpenClSnapshot $entries 'CurrentUser')
$desired = @($entries | ForEach-Object {[pscustomobject]@{View=$_.View; Name=$_.Name; Present=$true; Kind='DWord'; Value=0}})
try {
    Set-OpenClSnapshot $desired 'CurrentUser'
    if (!(Test-OpenClSnapshot $desired 'CurrentUser')) { throw 'Registration typed roundtrip failed' }
    $desired[0].Value = 1
    if (Test-OpenClSnapshot $desired 'CurrentUser') { throw 'Stale-state guard failed' }
    Set-OpenClSnapshot $before 'CurrentUser'
    if (!(Test-OpenClSnapshot $before 'CurrentUser')) { throw 'Rollback failed' }
    Write-Output 'PASS typed vendor entries, concurrent-state guard and original-value rollback'
} finally { Set-OpenClSnapshot $before 'CurrentUser' }
