# SPDX-License-Identifier: BSD-3-Clause
[CmdletBinding()]
param([Parameter(Mandatory)][string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$signer = '8705CD4DDD6DA49685EB34430106FA543FB59D53'
$root = (Resolve-Path -LiteralPath $PackageDirectory).Path
$expected = @('viogpuvideo.inf','viogpuvideo.sys','viogpuvideo_mft.dll','viogpuvideo-package.json','viogpuvideo.cat')
$items = @(Get-ChildItem -LiteralPath $root -Force)
if (@($items | Where-Object PSIsContainer).Count -or
    @($items | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count -or
    (Compare-Object @($expected | Sort-Object) @($items.Name | Sort-Object))) {
    throw 'VPU driver package must contain exactly the five flat installation files'
}
$cat = Join-Path $root 'viogpuvideo.cat'
$signature = Get-AuthenticodeSignature -LiteralPath $cat
if ($signature.Status -ne 'Valid' -or !$signature.SignerCertificate -or
    $signature.SignerCertificate.Thumbprint -cne $signer) { throw 'VPU catalog signer/trust validation failed' }

# Same documented WinVerifyTrust catalog-member path used by the shared installer,
# kept self-contained so this signed script does not execute an unsigned helper.
if (-not ('DroidVmVpuCatalog' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class DroidVmVpuCatalog {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct CatalogInfo {
        public uint Size, Version;
        [MarshalAs(UnmanagedType.LPWStr)] public string Catalog, Tag, Member;
        public IntPtr File, Hash;
        public uint HashLength;
        public IntPtr CatalogContext, Admin;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct TrustData {
        public uint Size;
        public IntPtr Policy, Sip;
        public uint UI, Revocation, Choice;
        public IntPtr Info;
        public uint StateAction;
        public IntPtr State, Url;
        public uint ProviderFlags, UIContext;
        public IntPtr SignatureSettings;
    }
    [DllImport("wintrust.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CryptCATAdminAcquireContext2(out IntPtr context, IntPtr subsystem,
        string algorithm, IntPtr policy, uint flags);
    [DllImport("wintrust.dll", SetLastError = true)]
    static extern bool CryptCATAdminCalcHashFromFileHandle2(IntPtr context, IntPtr file,
        ref uint size, byte[] hash, uint flags);
    [DllImport("wintrust.dll")]
    static extern bool CryptCATAdminReleaseContext(IntPtr context, uint flags);
    [DllImport("wintrust.dll", ExactSpelling = true)]
    static extern int WinVerifyTrust(IntPtr window, ref Guid action, ref TrustData data);
    public static void Verify(string catalog, string member) {
        IntPtr admin = IntPtr.Zero, info = IntPtr.Zero;
        GCHandle pinned = new GCHandle();
        var data = new TrustData(); bool called = false;
        var action = new Guid("00AAC56B-CD44-11D0-8CC2-00C04FC295EE");
        using (var file = File.Open(member, FileMode.Open, FileAccess.Read, FileShare.Read)) {
            try {
                if (!CryptCATAdminAcquireContext2(out admin, IntPtr.Zero, "SHA256", IntPtr.Zero, 0))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Catalog context");
                uint size = 0; var handle = file.SafeFileHandle.DangerousGetHandle();
                if (!CryptCATAdminCalcHashFromFileHandle2(admin, handle, ref size, null, 0))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Catalog hash size");
                var hash = new byte[size];
                if (!CryptCATAdminCalcHashFromFileHandle2(admin, handle, ref size, hash, 0))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Catalog hash");
                pinned = GCHandle.Alloc(hash, GCHandleType.Pinned);
                var cat = new CatalogInfo {
                    Size = (uint)Marshal.SizeOf(typeof(CatalogInfo)), Catalog = Path.GetFullPath(catalog),
                    Tag = BitConverter.ToString(hash).Replace("-", ""), Member = Path.GetFullPath(member),
                    File = handle, Hash = pinned.AddrOfPinnedObject(), HashLength = size, Admin = admin
                };
                info = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(CatalogInfo)));
                Marshal.StructureToPtr(cat, info, false);
                data = new TrustData {
                    Size = (uint)Marshal.SizeOf(typeof(TrustData)), UI = 2, Choice = 2,
                    Info = info, StateAction = 1, ProviderFlags = 0x1000
                };
                called = true;
                int status = WinVerifyTrust(new IntPtr(-1), ref action, ref data);
                if (status != 0) throw new InvalidOperationException("Catalog member failed 0x" +
                    unchecked((uint)status).ToString("X8") + ": " + member);
            } finally {
                if (called) { data.StateAction = 2; WinVerifyTrust(new IntPtr(-1), ref action, ref data); }
                if (info != IntPtr.Zero) {
                    Marshal.DestroyStructure(info, typeof(CatalogInfo)); Marshal.FreeHGlobal(info);
                }
                if (pinned.IsAllocated) pinned.Free();
                if (admin != IntPtr.Zero) CryptCATAdminReleaseContext(admin, 0);
            }
        }
    }
}
'@
}
$inventoryPath = Join-Path $root 'viogpuvideo-package.json'
[DroidVmVpuCatalog]::Verify($cat, $inventoryPath)
$inventory = Get-Content -LiteralPath $inventoryPath -Raw | ConvertFrom-Json
if ($inventory.schema -ne 1 -or $inventory.kind -cne 'viogpuvideo-arm64' -or
    $inventory.hardware_id -cne 'PCI\VEN_1AF4&DEV_1070' -or
    $inventory.signer_thumbprint -cne $signer -or $inventory.source_commit -notmatch '^[0-9a-f]{40}$' -or
    $inventory.source_parent -notmatch '^[0-9a-f]{40}$' -or $inventory.driver_version -notmatch '^0\.2\.0\.\d+$' -or
    $inventory.system_mft_registered -cne $false -or $inventory.dxva_advertised -cne $false) {
    throw 'VPU package identity mismatch'
}
$members = @('viogpuvideo.inf','viogpuvideo.sys','viogpuvideo_mft.dll')
if (Compare-Object @($members | Sort-Object) @($inventory.files.PSObject.Properties.Name | Sort-Object)) {
    throw 'Unexpected signed VPU file inventory'
}
foreach ($name in $members) {
    $file = Join-Path $root $name
    [DroidVmVpuCatalog]::Verify($cat, $file)
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -cne $inventory.files.$name) {
        throw "Signed VPU hash mismatch: $name"
    }
    if ([IO.Path]::GetExtension($name) -in @('.sys','.dll')) {
        $signed = Get-AuthenticodeSignature -LiteralPath $file
        if ($signed.Status -ne 'Valid' -or !$signed.SignerCertificate -or
            $signed.SignerCertificate.Thumbprint -cne $signer) { throw "Invalid signed VPU binary: $name" }
        $bytes = [IO.File]::ReadAllBytes($file)
        if ($bytes.Length -lt 64 -or [BitConverter]::ToUInt16($bytes,0) -ne 0x5a4d) { throw "Invalid PE: $name" }
        $pe = [BitConverter]::ToInt32($bytes,0x3c)
        if ($pe -lt 64 -or $pe -gt $bytes.Length-6 -or [BitConverter]::ToUInt32($bytes,$pe) -ne 0x4550 -or
            [BitConverter]::ToUInt16($bytes,$pe+4) -ne 0xaa64) { throw "VPU file is not ARM64 PE: $name" }
    }
}
$inf = Get-Content -LiteralPath (Join-Path $root 'viogpuvideo.inf') -Raw
if ($inf -notmatch '(?m)^Class=Media\s*$' -or $inf -notmatch [regex]::Escape($inventory.hardware_id) -or
    $inf -notmatch ('(?m)^DriverVer=[^,]+,' + [regex]::Escape($inventory.driver_version) + '\s*$') -or
    $inf -match 'RegisterDlls|CLSID|MFTRegister|DEV_1050|viogpuwddm') { throw 'Unexpected VPU INF binding or registration' }
[pscustomobject]@{Verified=$true;DriverVersion=$inventory.driver_version;SourceCommit=$inventory.source_commit;
    SourceParent=$inventory.source_parent;Signer=$signer;CatalogMembers=4;Directory=$root}
