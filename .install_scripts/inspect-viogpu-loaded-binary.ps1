$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$service = Get-CimInstance -ClassName Win32_SystemDriver -Filter "Name='VioGpuWddm'"
if ($null -eq $service) {
    throw 'The VioGpuWddm system driver service is missing.'
}

$imagePath = [Environment]::ExpandEnvironmentVariables([string]$service.PathName).Trim('"')
if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
    $imagePath = Join-Path $env:SystemRoot $imagePath.Substring('\SystemRoot\'.Length)
}
elseif ($imagePath.StartsWith('System32\', [StringComparison]::OrdinalIgnoreCase)) {
    $imagePath = Join-Path $env:SystemRoot $imagePath
}

$file = Get-Item -LiteralPath $imagePath
$signature = Get-AuthenticodeSignature -LiteralPath $imagePath
[pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    Service = [pscustomobject]@{
        Name = $service.Name
        State = $service.State
        Status = $service.Status
        Started = $service.Started
        StartMode = $service.StartMode
        RawPathName = $service.PathName
    }
    ImagePath = $file.FullName
    ImageLength = $file.Length
    ImageLastWriteTimeUtc = $file.LastWriteTimeUtc.ToString('o')
    ImageSha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    FileVersion = $file.VersionInfo.FileVersion
    SignatureStatus = [string]$signature.Status
    SignerSubject = if ($null -eq $signature.SignerCertificate) { $null } else { $signature.SignerCertificate.Subject }
} | ConvertTo-Json -Depth 5
