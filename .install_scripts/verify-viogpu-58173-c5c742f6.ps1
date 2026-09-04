[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\USER\viogpu-58173-c5c742f6\package',
    [string]$CertificatePath = 'C:\Users\USER\viogpu-58173-c5c742f6\DroidVM_Test.cer',
    [string]$OutputPath = 'C:\Users\USER\viogpu-58173-c5c742f6\preinstall-verification.json'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedHashes = [ordered]@{
    'viogpud3d.dll' = '7a26660fedbbe37a52d8a1235541e72817342668955f2a7bca17fbe73da8ea7b'
    'viogpud3d.pdb' = '768dcc6917119f0c06c7f62e3d230d8e1a4ff497b813ed4644eb6cf3258bd24a'
    'viogpuwddm.cat' = 'acff8e7a8aafe8ffc1d37b7f2f661bd62c2792f8e01cd86930ad17e65d72a935'
    'viogpuwddm.inf' = '26e296c2003709243bae22f4311fc00fd49f620f2b37bb9abb0df2706ebb77f0'
    'viogpuwddm.map' = 'ff1698990d46fa94f8a338295fe021862fad38e12676c2a0c357273471ba2d1a'
    'viogpuwddm.pdb' = 'fbc9f8818fe340144c63f5c325f3e7200b25d4c9c3f926a3a86965f68b357ea8'
    'viogpuwddm.sys' = '6d5705699873b9cc1061e701ddfa8246dd6143051022ce7b8b1d1cbaafbdc8dd'
}
$expectedCertificateHash = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
$expectedSignerSubject = 'CN=DroidVM Test'
$expectedVersion = '100.6.101.58173'

if (-not (Test-Path -LiteralPath $PackageRoot -PathType Container)) {
    throw "Missing package directory: $PackageRoot"
}
if (-not (Test-Path -LiteralPath $CertificatePath -PathType Leaf)) {
    throw "Missing package certificate: $CertificatePath"
}

$certificateHash = (Get-FileHash -LiteralPath $CertificatePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($certificateHash -ne $expectedCertificateHash) {
    throw "Certificate SHA-256 mismatch: expected $expectedCertificateHash, got $certificateHash"
}
$expectedCertificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($CertificatePath)

$verifiedHashes = [ordered]@{}
foreach ($entry in $expectedHashes.GetEnumerator()) {
    $path = Join-Path $PackageRoot $entry.Key
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing package file: $path"
    }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $entry.Value) {
        throw "SHA-256 mismatch for '$path': expected $($entry.Value), got $actual"
    }
    $verifiedHashes[$entry.Key] = $actual
}

$unexpectedFiles = @(
    Get-ChildItem -LiteralPath $PackageRoot -File |
        Where-Object { -not $expectedHashes.Contains($_.Name) }
)
if ($unexpectedFiles.Count -ne 0) {
    throw "Unexpected package files: $($unexpectedFiles.Name -join ', ')"
}

$infPath = Join-Path $PackageRoot 'viogpuwddm.inf'
$infText = Get-Content -LiteralPath $infPath -Raw
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58173') {
    throw "The staged INF is not version $expectedVersion"
}
if ($infText -notmatch 'PCI\\VEN_1AF4&DEV_1050' -or
    $infText -match '(?i)netkvm|rdmapool|droidvmpool|DRVM0001') {
    throw 'The staged INF does not describe only the expected virtio-gpu device.'
}

$signatures = [ordered]@{}
foreach ($fileName in @('viogpuwddm.sys', 'viogpud3d.dll', 'viogpuwddm.cat')) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $PackageRoot $fileName)
    $signer = $signature.SignerCertificate
    if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
        $null -eq $signer -or
        $signer.Subject -ne $expectedSignerSubject -or
        $signer.Thumbprint -ne $expectedCertificate.Thumbprint) {
        throw "Invalid signature for '$fileName': status=$($signature.Status), signer=$($signer.Subject)"
    }
    $signatures[$fileName] = [ordered]@{
        Status = [string]$signature.Status
        Subject = $signer.Subject
        Thumbprint = $signer.Thumbprint
    }
}

$result = [ordered]@{
    VerifiedAt = (Get-Date).ToString('o')
    PackageRoot = $PackageRoot
    PackageVersion = $expectedVersion
    CertificateSha256 = $certificateHash
    CertificateThumbprint = $expectedCertificate.Thumbprint
    Hashes = $verifiedHashes
    Signatures = $signatures
}
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
$result | ConvertTo-Json -Depth 6
