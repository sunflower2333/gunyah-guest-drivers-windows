# Gunyah Windows guest drivers for DroidVM

This fork ports the Windows guest drivers to the **Gunyah protected VM**
platform (DroidVM: Windows 11 ARM64 running under crosvm + the Gunyah
hypervisor on an Android phone), on top of upstream virtio-win.

## How it works

The DroidVM Native Context product path targets an unprotected VM. Ordinary
VirtIO buffers use the upstream nonpaged/contiguous allocation path, while the
full viogpu miniport receives ordinary guest RAM from VidMm and describes those
pages to VirtIO GPU with standard scatter/gather backing.

- **WDF drivers** (vioinput, ...) are routed centrally by
  `VirtIO/WDF` (Dma.c / VirtIOWdf.c).
- **viogpu** contains the Windows Native Context full-miniport source for the
  Turnip/crosvm path. `RenderOnly=1` selects a Win8/WDDM 1.2 render-only
  registration with zero display topology; `RenderOnly=0` preserves the
  legacy Win7/WDDMv1 display registration. The conditional registration change
  has only local source/ABI validation; Windows/KMT/Host/GPU runtime remains
  unverified, so no package containing it is approved for installation.
- **PortCls** (viosnd) has a single DMA allocator to redirect, so
  `viosnd/sys/ViosndRdma.cpp` sub-allocates the pool page by page and
  `ViosndAllocateDmaBuffer` draws from it. The WaveRT buffer the Windows
  audio engine writes into stays ordinary guest memory -- it is never
  device-visible, because each period is copied into a pooled
  `VIRTIO_SND_PCM_XFER` request before the descriptor is added.

Every pVM path is gated on the presence of the `ACPI\RDMA0000` device
interface: the same disk image falls back to the stock virtio paths on
QEMU/KVM.

## Driver status

Legend:
* ✨ new driver added by this fork
* ✅ ported and verified
* ✨ new driver added by this fork 
* ✅ ported and verified 
* ⚠️ ported but not yet verified 
* ❌ not ported
* 🚫 explicitly unsupported and rejected

| Driver | Status | Notes |
|---|:---:|---|
| pvmpower.sys | ✨ | PSCI shutdown/reboot bridge<br> detect S5 `ShutdownType` and launch gunyah hypercall to shutdown/restart the VM |
| viostor | ✅ | stock VirtIO path |
| NetKVM | ✅ | stock VirtIO path |
| vioinput | ✅ | via the VirtIO-WDF routing; in daily use (VNC input) |
| vioscsi | ✅ | stock VirtIO path |
| vioserial | ⚠️ | VirtIO-WDF routing in place, untested on a pVM |
| viorng | ⚠️ | VirtIO-WDF routing in place, untested on a pVM |
| viosock | ⚠️ | VirtIO-WDF routing in place, untested on a pVM |
| Balloon | ⚠️ | VirtIO-WDF routing in place, untested on a pVM |
| viomem | ⚠️ | VirtIO-WDF routing in place, untested on a pVM |
| viofs | ⚠️ | VirtIO-WDF routing in place, data path unreviewed |
| viogpu | ⚠️ | Native Context full miniport has a locally validated conditional WDDM 1.2 render-only path and remains runtime-unverified/do-not-install |
| viofs | ✅ | VirtIO-WDF routing + the FUSE payload staged through the rdmapool; ships with `virtiofs.exe`, the WinFsp service that does the mounting, and shared folders work in the DroidVM app |
| viosnd | ✅ | **not from upstream virtio-win** -- virtio-win has no sound driver ([issue #929](https://github.com/virtio-win/kvm-guest-drivers-windows/issues/929) is still open). Ported from [317764920/viosnd](https://github.com/317764920/viosnd) (BSD-3, PortCls + WaveRT over the low-level VirtioLib), with the pVM staging in `ViosndRdma.{h,cpp}`. Playback and capture both work in the DroidVM app. |
| viogpu | ❌ | not ported; need huge works(~~dxvk~~ -> ~~gfxstream~~ -> Turnip Driver -> AHardwareBuffer) |
| pvpanic | ❌ | not ported |
| fwcfg  | ❌ | not ported |
| ivshmem | ❌ | not ported |
| viocrypt | ❌ | not ported |
| pciserial | ❌ | not ported |


---

# KVM/QEMU Windows guest drivers (virtio-win) #

This repository contains KVM/QEMU Windows guest drivers, for both
paravirtual and emulated hardware. The code builds and ships as part
of the virtio-win RPM on Fedora and Red Hat Enterprise Linux, and the
binaries are also available in the form of distribution-neutral ISO
and VFD images. If all you want is use virtio-win in your Windows
virtual machines, go to the
[Fedora virtIO-win documentation][fedora-virtio]
for information on obtaining the binaries.

If you'd like to build virtio-win from sources, clone this repo and
follow the instructions in [Building the Drivers][wiki-building].
Note that the drivers you build will be either unsigned or test-signed
with Tools/VirtIOTestCert.cer, which means that Windows will not load
them by default. See [Microsoft's driver signing page][ms-signing]
for more information on test-signing.

If you want to build cross-signed binaries (like the ones that ship in
the Fedora RPM), you'll need your own code-signing certificate.
Cross-signed drivers can be used on all versions of Windows except for
the latest Windows 10 with secure boot enabled. However, systems with
cross-signed drivers will not receive Microsoft support.

If you want to produce Microsoft-signed binaries (fully supported,
like the ones that ship in the Red Hat Enterprise Linux RPM), you'll
need to submit the drivers to Microsoft along with a set of test
results (so called WHQL process). If you decide to WHQL the drivers,
make sure to base them on commit eb2996de or newer, since the GPL
license used prior to this commit is not compatible with WHQL.
Additionally, we ask that you make a change to the Hardware IDs so
that your drivers will *not* match devices exposed by the upstream
versions of KVM/QEMU. This is especially important if you plan to
distribute the drivers with Windows Update, see the 
[Microsoft publishing restrictions][ms-publishing] for more details.

[fedora-virtio]:https://docs.fedoraproject.org/en-US/quick-docs/creating-windows-virtual-machines-using-virtio-drivers/index.html
[wiki-building]:https://virtio-win.github.io/Development/Building-the-drivers-using-Windows-11-24H2-EWDK
[ms-signing]:https://docs.microsoft.com/en-us/windows-hardware/drivers/install/installing-test-signed-driver-packages
[ms-publishing]:https://docs.microsoft.com/en-us/windows-hardware/drivers/dashboard/publishing-restrictions
- - - -
