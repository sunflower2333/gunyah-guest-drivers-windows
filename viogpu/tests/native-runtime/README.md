# Microsoft runtime VIOGPU probes

These are ordinary Windows applications. They dynamically use the Microsoft
System32 runtime, select exactly one hardware adapter with PCI ID 1AF4:1050,
and require the expected installed UMD to be loaded by the runtime with an
exact path and SHA256. They do not call private driver harness exports or
replace d3d11.dll, d3d12.dll or dxgi.dll beside the executable.

Run from an ordinary directory in the interactive desktop session, with the
matching architecture and a bounded outer process timeout. Example:

```powershell
.\runtime-probe-arm64.exe --api d3d11 --expect-umd 'C:\Windows\System32\DriverStore\FileRepository\<installed-package>\viogpudxvk.dll' --expect-umd-sha256 '<actual signed DLL SHA256>'
```

The default minimum feature level is11_0. To exercise an explicitly lower
bring-up milestone, pass `--minimum-feature-level 10_0` (10_1 and11_1 are also
accepted). The output records both required and actual levels and rejects an
unexpected downgrade. D3D12 requires at least11_0. A lower bring-up pass does
not satisfy higher feature-level acceptance; device creation at a level alone
also does not prove that all features of that level work correctly.

The D3D11 case creates a runtime device, compiles HLSL and performs four
alternating red/green draws over a blue clear. All16384 pixels are checked
through staging readback, and four windowed Present calls must return S_OK.
The output records the actual feature level; use of D3D11 at FL10_x does not
establish support for FL11_x features. Successful Present is runtime acceptance,
not proof that the final Android display showed correct pixels. That requires
the separate target display observation and desktop stability checks.

The D3D12 case checks only real D3D12CreateDevice activation, loaded UMD,
adapter LUID and device-loss status. It explicitly does not claim D3D12 draw,
queue, residency, fence or Present validation.

CI builds native ARM64, AMD64 and x86 executables and runs each architecture
on Windows ARM64 with `--self-test-warp`. This mode uses Microsoft's software
adapter to validate the probe itself and cannot emit a hardware pass. Negative
cases reject missing hardware UMD identity and attempts to mix WARP with a
hardware identity. No target device or VIOGPU driver is installed in CI.
