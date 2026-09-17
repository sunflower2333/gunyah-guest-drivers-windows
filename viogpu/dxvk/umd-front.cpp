// SPDX-License-Identifier: MIT
// ARM64X entry for the DXVK D3D10/11 candidate user-mode driver. No rendering
// code: the native view loads viogpudxvk.dll and the ARM64EC view, which serves
// emulated x64 processes, loads viogpudxvk_x64.dll, each by exact sibling path.
// 32-bit processes would load viogpudxvk_x86.dll through UserModeDriverNameWow.
#define VIOGPU_UMD_NATIVE_TARGET L"viogpudxvk.dll"
#define VIOGPU_UMD_EC_TARGET L"viogpudxvk_x64.dll"
#include "../d3d10/umd-front.cpp"
