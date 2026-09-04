$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$device = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
$signed = @(Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceID -like 'PCI\VEN_1AF4&DEV_1050*' })
$driverPath = Join-Path $env:SystemRoot 'System32\drivers\viogpuwddm.sys'
$bcd = @(bcdedit.exe /enum '{current}' 2>&1 | Select-String -Pattern 'testsigning|nointegritychecks')

[pscustomobject]@{
    Machine = [Environment]::MachineName
    Device = $device | Select-Object Status, ProblemCode, InstanceId, FriendlyName
    SignedDriver = $signed | Select-Object DriverVersion, InfName, DriverProviderName, DriverDate
    DriverPath = $driverPath
    DriverFile = if (Test-Path -LiteralPath $driverPath) {
        Get-Item -LiteralPath $driverPath | Select-Object FullName, Length, LastWriteTime
    } else { $null }
    DriverHash = if (Test-Path -LiteralPath $driverPath) {
        (Get-FileHash -LiteralPath $driverPath -Algorithm SHA256).Hash.ToLowerInvariant()
    } else { $null }
    BootOptions = $bcd | ForEach-Object { $_.Line.Trim() }
} | ConvertTo-Json -Depth 6
