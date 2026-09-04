[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\USER\viogpu-58171-ed14bdae\package',
    [string]$CertificatePath = 'C:\Users\USER\viogpu-58171-ed14bdae\DroidVM_Test.cer',
    [string]$OutputPath = 'C:\Users\USER\viogpu-58171-ed14bdae\preinstall-verification.json'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedHashes = [ordered]@{
    'viogpud3d.dll' = '791a9e11549a58b2df66441f7d6df36c1bf261860bca433c1aa031087e80872f'
    'viogpud3d.pdb' = '3ef40b680722dff02a3268a523f1f104487f988c9a18c1c3ad351d35a864a0b1'
    'viogpuwddm.cat' = '907ff9059537b0ffff8f2168074087869916e7cc4b67177e717378a9c03f6ed2'
    'viogpuwddm.inf' = 'a8f11df98fcb82bb2695bc4a233e87976e8ec781b23e82df5a1abb7e6a8c0bfd'
    'viogpuwddm.map' = '31671c97c95fe6179f49fba38c5cd5f14a86ad4f3e8349fcfe217879b782ff44'
    'viogpuwddm.pdb' = 'f58cf006b8634de24fe315e60b1685ccc1dc31f80479e3ff4f002067051a4407'
    'viogpuwddm.sys' = '3d3595baaecb92fdbf3f3b7907459dfc64978b5c2a9c57b14956040f6ff88ed0'
}
$expectedCertificateHash = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
$expectedSignerSubject = 'CN=DroidVM Test'
$expectedVersion = '100.6.101.58171'

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
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58171') {
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
