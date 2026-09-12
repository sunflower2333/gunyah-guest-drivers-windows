# SPDX-License-Identifier: MIT
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security

function Get-GpuCertificateHash($Certificate) {
    $hash = [Security.Cryptography.SHA256]::Create()
    try { ([BitConverter]::ToString($hash.ComputeHash($Certificate.RawData))).Replace('-','').ToLowerInvariant() }
    finally { $hash.Dispose() }
}

function Assert-GpuBundledCertificate([string]$CertificatePath,[string]$CatalogPath) {
    $certificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2(,
        [IO.File]::ReadAllBytes($CertificatePath))
    $cms = New-Object Security.Cryptography.Pkcs.SignedCms
    try {
        $cms.Decode([IO.File]::ReadAllBytes($CatalogPath))
        # Verify the actual catalog signature without accepting its trust chain.
        $cms.CheckSignature($true)
        if ($cms.SignerInfos.Count -ne 1 -or $null -eq $cms.SignerInfos[0].Certificate -or
            (Get-GpuCertificateHash $cms.SignerInfos[0].Certificate) -cne (Get-GpuCertificateHash $certificate)) {
            throw 'Bundled certificate does not exactly match the catalog signer'
        }
        if ($certificate.HasPrivateKey) { throw 'Installer certificate must be public only' }
        return $certificate
    } catch { $certificate.Dispose(); throw }
}

function Add-GpuPackageTrust($Certificate,$State,[string]$Journal,
    [ValidateSet('LocalMachine','CurrentUser')][string]$Location='LocalMachine') {
    foreach ($name in @('Root','TrustedPublisher')) {
        $record = [pscustomobject]@{Store=$name;Location=$Location;Thumbprint=$Certificate.Thumbprint;
            Hash=(Get-GpuCertificateHash $Certificate);Status='adding';Created=$false}
        $State.CertificateTrust += $record
        Write-GpuJournal $State $Journal
        $record.Created = [DroidVmGpuInstall.Native]::AddCertificateNew($name,$Certificate.RawData,($Location -eq 'LocalMachine'))
        $record.Status = if ($record.Created) {'created'} else {'preexisting'}
        Write-GpuJournal $State $Journal
    }
}

function Remove-GpuAttemptTrust($Records) {
    foreach ($record in $Records) {
        if ($record.Status -eq 'adding') { throw 'Interrupted certificate creation has uncertain ownership; keep journal' }
        if (!$record.Created -or $record.Status -ne 'created') { continue }
        if ($record.Store -notin @('Root','TrustedPublisher') -or
            $record.Location -notin @('LocalMachine','CurrentUser')) { throw 'Unexpected saved certificate store' }
        $store = New-Object Security.Cryptography.X509Certificates.X509Store($record.Store,$record.Location)
        try {
            $store.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
            $matches = @($store.Certificates | Where-Object Thumbprint -ceq $record.Thumbprint)
            foreach ($cert in $matches) {
                if ((Get-GpuCertificateHash $cert) -cne $record.Hash) { throw 'Saved certificate identity changed' }
                $store.Remove($cert)
            }
        } finally { $store.Close() }
    }
}

Export-ModuleMember -Function Get-GpuCertificateHash,Assert-GpuBundledCertificate,Add-GpuPackageTrust,Remove-GpuAttemptTrust
