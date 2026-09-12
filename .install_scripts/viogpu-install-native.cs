// SPDX-License-Identifier: MIT
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace DroidVmGpuInstall {
    public static class Native {
        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool SetupCopyOEMInfW(string source, string location, uint media,
            uint flags, StringBuilder destination, uint size, out uint required, IntPtr component);
        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool SetupGetInfDriverStoreLocationW(string inf, IntPtr platform,
            string locale, StringBuilder result, uint size, out uint required);
        [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool DiInstallDriverW(IntPtr window, string inf, uint flags, out bool reboot);
        [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool DiUninstallDriverW(IntPtr window, string inf, uint flags, out bool reboot);

        public static string Stage(string inf) {
            var result = new StringBuilder(32768); uint needed;
            if (!SetupCopyOEMInfW(Path.GetFullPath(inf), null, 1, 0, result,
                (uint)result.Capacity, out needed, IntPtr.Zero))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "SetupCopyOEMInfW");
            return result.ToString();
        }
        public static string StoreInf(string inf) {
            var result = new StringBuilder(32768); uint needed;
            if (!SetupGetInfDriverStoreLocationW(inf, IntPtr.Zero, null, result,
                (uint)result.Capacity, out needed))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "SetupGetInfDriverStoreLocationW");
            return result.ToString();
        }
        public static bool Install(string inf, bool rollback) {
            bool reboot;
            if (!DiInstallDriverW(IntPtr.Zero, Path.GetFullPath(inf), rollback ? 2u : 0u, out reboot))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "DiInstallDriverW");
            return reboot;
        }
        public static bool Remove(string inf) {
            bool reboot;
            if (!DiUninstallDriverW(IntPtr.Zero, Path.GetFullPath(inf), 0, out reboot))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "DiUninstallDriverW");
            return reboot;
        }

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

        // Authenticates INF/manifest/runtime membership in the signed driver CAT;
        // Get-AuthenticodeSignature of the CAT alone cannot verify membership.
        public static void VerifyCatalogMember(string catalog, string member) {
            IntPtr admin = IntPtr.Zero, info = IntPtr.Zero;
            GCHandle pinned = new GCHandle();
            var data = new TrustData(); bool trustCalled = false;
            var action = new Guid("00AAC56B-CD44-11D0-8CC2-00C04FC295EE");
            using (var file = File.Open(member, FileMode.Open, FileAccess.Read, FileShare.Read)) {
                try {
                    if (!CryptCATAdminAcquireContext2(out admin, IntPtr.Zero, "SHA256", IntPtr.Zero, 0))
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "CryptCATAdminAcquireContext2");
                    uint size = 0; var handle = file.SafeFileHandle.DangerousGetHandle();
                    if (!CryptCATAdminCalcHashFromFileHandle2(admin, handle, ref size, null, 0))
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "Catalog hash size");
                    var hash = new byte[size];
                    if (!CryptCATAdminCalcHashFromFileHandle2(admin, handle, ref size, hash, 0))
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "Catalog member hash");
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
                    trustCalled = true;
                    int status = WinVerifyTrust(new IntPtr(-1), ref action, ref data);
                    if (status != 0)
                        throw new InvalidOperationException("Catalog membership failed 0x" +
                            unchecked((uint)status).ToString("X8") + ": " + member);
                } finally {
                    if (trustCalled) { data.StateAction = 2; WinVerifyTrust(new IntPtr(-1), ref action, ref data); }
                    if (info != IntPtr.Zero) {
                        Marshal.DestroyStructure(info, typeof(CatalogInfo)); Marshal.FreeHGlobal(info);
                    }
                    if (pinned.IsAllocated) pinned.Free();
                    if (admin != IntPtr.Zero) CryptCATAdminReleaseContext(admin, 0);
                }
            }
        }
    }
}
