[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-58050',
    [string]$CertificatePath = 'C:\Users\Administrator\DroidVM_Test.cer'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$infPath = Join-Path $PackageRoot 'viogpuwddm.inf'
if (-not (Test-Path -LiteralPath $infPath -PathType Leaf)) {
    throw "Missing viogpu INF: $infPath"
}

if (Test-Path -LiteralPath $CertificatePath -PathType Leaf) {
    Import-Certificate -FilePath $CertificatePath -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
    Import-Certificate -FilePath $CertificatePath -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null
}

$before = @(
    Get-CimInstance Win32_PnPSignedDriver |
        Where-Object { $_.DeviceID -like 'PCI\VEN_1AF4&DEV_1050*' } |
        Select-Object DeviceID,DriverVersion,InfName,DriverDate,Manufacturer
)
$output = @(& pnputil.exe /add-driver $infPath /install 2>&1)
$exitCode = $LASTEXITCODE
if ($exitCode -notin @(0, 3010)) {
    throw "pnputil failed with exit code $exitCode`n$($output -join [Environment]::NewLine)"
}
Start-Sleep -Seconds 3
$after = @(
    Get-CimInstance Win32_PnPSignedDriver |
        Where-Object { $_.DeviceID -like 'PCI\VEN_1AF4&DEV_1050*' } |
        Select-Object DeviceID,DriverVersion,InfName,DriverDate,Manufacturer
)
$device = @(
    Get-PnpDevice -PresentOnly |
        Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' } |
        Select-Object Status,Class,FriendlyName,InstanceId
)

[pscustomobject]@{
    PackageRoot = $PackageRoot
    InfPath = $infPath
    PnpUtilExitCode = $exitCode
    PnpUtilOutput = $output
    Before = $before
    After = $after
    Device = $device
    RebootRequired = ($exitCode -eq 3010)
} | ConvertTo-Json -Depth 6
