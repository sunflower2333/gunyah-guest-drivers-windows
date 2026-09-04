[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-58053'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expected = [ordered]@{
    'viogpuwddm.sys' = 'c29d4e8e360dc4cf9410f28825e776306f29aede899d161bceb2c6a9de7bfdff'
    'viogpud3d.dll' = 'b7a509b35e506400070e2adbf67012d227ebf4477f88a6688f2b3336734d8cf4'
    'viogpuwddm.cat' = '583e2212aab39dc38e680bfcbe8e645eb7f4d6594c7a5217323fc80712e90dae'
}

$inf = Join-Path $PackageRoot 'viogpuwddm.inf'
$cert = Join-Path $PackageRoot 'DroidVM_Test.cer'
if (-not (Test-Path -LiteralPath $inf)) { throw "Missing INF: $inf" }
if (-not (Test-Path -LiteralPath $cert)) { throw "Missing certificate: $cert" }

foreach ($entry in $expected.GetEnumerator()) {
    $path = Join-Path $PackageRoot $entry.Key
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing package file: $path" }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $entry.Value) { throw "Hash mismatch for $($entry.Key): $actual" }
}

$infText = Get-Content -LiteralPath $inf -Raw
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58053') {
    throw 'Unexpected package version'
}

Import-Certificate -FilePath $cert -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
Import-Certificate -FilePath $cert -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null
$before = @(Get-PnpDevice -PresentOnly:$false | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' } | Select-Object Status,Problem,ProblemCode,Class,FriendlyName,InstanceId)
$output = @(& pnputil.exe /add-driver $inf /install 2>&1)
$exitCode = $LASTEXITCODE
if ($exitCode -notin @(0, 3010)) { throw "pnputil failed ($exitCode): $($output -join "`n")" }
Start-Sleep -Seconds 3
$after = @(Get-PnpDevice -PresentOnly:$false | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' } | Select-Object Status,Problem,ProblemCode,Class,FriendlyName,InstanceId)
[pscustomobject]@{
    PackageVersion = '100.6.101.58053'
    PnpUtilExitCode = $exitCode
    PnpUtilOutput = $output
    Before = $before
    After = $after
} | ConvertTo-Json -Depth 7
