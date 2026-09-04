[CmdletBinding()]
param(
    [string]$PackageRoot = 'C:\Users\USER\viogpu-58180-00c0fd02\package',
    [string]$CertificatePath = 'C:\Users\USER\viogpu-58180-00c0fd02\DroidVM_Test.cer',
    [string]$OutputPath = 'C:\Users\USER\viogpu-58180-00c0fd02\preinstall-verification.json'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedHashes = [ordered]@{
    'viogpud3d.dll' = '96f47b5544ec950b7166d0f85052b4f574328786a13143c3a12c4d94ca7eae5b'
    'viogpud3d.pdb' = '663c1c5e5d0c542ce55169293470d1970ab94e85c0237fedc70a7f52caa8f66e'
    'viogpuwddm.cat' = 'eab9057f6ee4348d61b2d9a4edfec513183e20c2cf0029cda5723badd5db636e'
    'viogpuwddm.inf' = '1d5866d3f6bd15f648fc79fd1a43f15c99e2098cc4af53b1b66f4033c54cea1d'
    'viogpuwddm.map' = 'dcb6005eaad826c382ea98aa36c1e709408160ee06841cd6f7cff3d8c5ea20b3'
    'viogpuwddm.pdb' = 'c6d9f43313369a4508a9932d979c3a4a72451cac5c2958c3d5e45d8fe6c500ef'
    'viogpuwddm.sys' = 'cbaa70090a150200b6a93f078be5a74ec302d222ef149cd35d06e7598b372524'
}
$expectedCertificateHash = 'da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3'
$expectedSignerSubject = 'CN=DroidVM Test'
$expectedVersion = '100.6.101.58180'

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
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58180') {
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
        $subject = if ($null -eq $signer) { '<none>' } else { $signer.Subject }
        throw "Invalid signature for '$fileName': status=$($signature.Status), signer=$subject"
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
