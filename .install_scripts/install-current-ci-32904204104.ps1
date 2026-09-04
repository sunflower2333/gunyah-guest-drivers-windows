[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\administrator\viogpu-ci-32904204104'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$inf = Join-Path $PackageRoot 'drivers\viogpu\viogpuwddm.inf'
$cert = Join-Path $PackageRoot 'DroidVM_Test.cer'
$sys = Join-Path $PackageRoot 'drivers\viogpu\viogpuwddm.sys'
$cat = Join-Path $PackageRoot 'drivers\viogpu\viogpuwddm.cat'
foreach ($path in @($inf, $cert, $sys, $cat)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing package file: $path"
    }
}

function Get-GpuState {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
    })
    $rows = foreach ($device in $devices) {
        $driver = $null
        try {
            $driver = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
        }
        catch {
            $driver = $null
        }
        [pscustomobject]@{
            Status = [string]$device.Status
            ProblemCode = if ($device.PSObject.Properties['ProblemCode']) { $device.ProblemCode } else { $null }
            InstanceId = [string]$device.InstanceId
            DriverKey = if ($driver) { [string]$driver.Data } else { $null }
        }
    }
    @($rows)
}

$before = @(Get-GpuState)
$certInfo = Get-AuthenticodeSignature -FilePath $sys
$catInfo = Get-AuthenticodeSignature -FilePath $cat
$rootCert = Import-Certificate -FilePath $cert -CertStoreLocation 'Cert:\LocalMachine\Root'
$publisherCert = Import-Certificate -FilePath $cert -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher'
$installOutput = @(& pnputil.exe /add-driver $inf /install 2>&1)
$installExitCode = $LASTEXITCODE
if ($installExitCode -notin @(0, 3010)) {
    throw "pnputil failed ($installExitCode): $($installOutput -join [Environment]::NewLine)"
}
Start-Sleep -Seconds 5
$after = @(Get-GpuState)
[pscustomobject]@{
    PackageRoot = $PackageRoot
    DriverSha256 = (Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash.ToLowerInvariant()
    CatalogSha256 = (Get-FileHash -LiteralPath $cat -Algorithm SHA256).Hash.ToLowerInvariant()
    DriverSignatureStatus = [string]$certInfo.Status
    CatalogSignatureStatus = [string]$catInfo.Status
    DriverSigner = if ($certInfo.SignerCertificate) { $certInfo.SignerCertificate.Thumbprint } else { $null }
    CatalogSigner = if ($catInfo.SignerCertificate) { $catInfo.SignerCertificate.Thumbprint } else { $null }
    RootCertificateThumbprint = $rootCert.Thumbprint
    TrustedPublisherThumbprint = $publisherCert.Thumbprint
    PnpUtilExitCode = $installExitCode
    PnpUtilOutput = $installOutput
    Before = $before
    After = $after
} | ConvertTo-Json -Depth 8

