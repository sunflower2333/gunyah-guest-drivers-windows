// SPDX-License-Identifier: MIT
// Loads the DXVK D3D10/11 user-mode driver a process of this architecture
// would use from a package directory, then checks three things on the shipped
// image without an adapter or a GPU:
//   - it is the architecture-matched DXVK build, with its adapter entry points;
//   - its admission gate is closed: the legacy OpenAdapter10 path refuses with
//     DXGI_ERROR_UNSUPPORTED before it touches runtime callbacks (an open gate
//     would reach them and fail with E_INVALIDARG instead);
//   - it resolves its Vulkan loader as the private viogpu_gl_loader_<arch>.dll
//     beside itself, never the application's vulkan-1.dll.
// No Vulkan instance, device or rendering: an ABI/load check only.
// d3d10umddi.h needs NTSTATUS, which the lean windows.h leaves out.
#include <windows.h>
#include <d3d10_1.h>
#include <d3d10umddi.h>
#include <cstdio>
#include <cwchar>
#include <string>

#if defined(_M_ARM64)
static const wchar_t arch[] = L"arm64";
static const wchar_t registered[] = L"viogpudxvk.dll";
static const wchar_t runtime[] = L"viogpudxvk.dll";
static const wchar_t loader[] = L"viogpu_gl_loader_arm64.dll";
static const WORD runtime_machine = IMAGE_FILE_MACHINE_ARM64;
#else
#error The DXVK UMD package probe is ARM64-only for now
#endif

static int fail(const wchar_t *what, unsigned long detail = GetLastError())
{
    std::fwprintf(stderr, L"FAIL %ls DXVK UMD: %ls (0x%08lx)\n", arch, what, detail);
    return 1;
}

static WORD image_machine(HMODULE module)
{
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(reinterpret_cast<const BYTE *>(module) + dos->e_lfanew);
    return nt->FileHeader.Machine;
}

static bool module_path(HMODULE module, const std::wstring &expected)
{
    wchar_t actual[32768];
    DWORD length = GetModuleFileNameW(module, actual, _countof(actual));
    return length && length < _countof(actual) && !_wcsicmp(actual, expected.c_str());
}

using OpenAdapterEntry = HRESULT(APIENTRY *)(D3D10DDIARG_OPENADAPTER *);
using QueryLoaderEntry = HRESULT(APIENTRY *)(WCHAR *, UINT32);

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2)
    {
        std::fwprintf(stderr, L"usage: dxvk-umd-probe-%ls.exe <package directory>\n", arch);
        return 2;
    }
    std::wstring root = argv[1];
    if (!root.empty() && root.back() != L'\\' && root.back() != L'/')
    {
        root += L'\\';
    }
    const std::wstring entry_path = root + registered;
    const std::wstring runtime_path = root + runtime;
    const std::wstring loader_path = root + loader;

    HMODULE entry = LoadLibraryExW(entry_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!entry)
    {
        return fail(L"entry does not load");
    }
    auto open10 = reinterpret_cast<OpenAdapterEntry>(GetProcAddress(entry, "OpenAdapter10"));
    if (!open10 || !GetProcAddress(entry, "OpenAdapter10_2"))
    {
        return fail(L"entry lacks OpenAdapter10/OpenAdapter10_2", 0);
    }
    HMODULE dxvk = entry;
    if (!module_path(dxvk, runtime_path))
    {
        return fail(L"DXVK UMD resolved from the wrong path", 0);
    }
    if (image_machine(dxvk) != runtime_machine)
    {
        return fail(L"DXVK UMD has the wrong architecture", image_machine(dxvk));
    }
    auto query = reinterpret_cast<QueryLoaderEntry>(GetProcAddress(dxvk, "VioGpuDxvkQueryVulkanLoader"));
    if (!query)
    {
        return fail(L"DXVK UMD lacks its Vulkan loader check", 0);
    }

    // The exact legacy negotiation the D3D10 runtime performs. The canary
    // proves the table outside D3D10DDI_ADAPTERFUNCS is never written.
    struct
    {
        D3D10DDI_ADAPTERFUNCS table;
        UINT64 canary;
    } legacy = {{}, 0x5a5aa5a5f00dcafeull};
    D3D10DDIARG_OPENADAPTER open = {};
    open.Interface = D3D10_0_DDI_INTERFACE_VERSION;
    open.Version = D3D10_0_DDI_BUILD_VERSION << 16;
    open.pAdapterFuncs = &legacy.table;
    const HRESULT gate = open10(&open);
    if (gate != DXGI_ERROR_UNSUPPORTED || open.hAdapter.pDrvPrivate || legacy.table.pfnCreateDevice ||
        legacy.canary != 0x5a5aa5a5f00dcafeull)
    {
        return fail(L"admission gate is not closed in the shipped image", static_cast<unsigned long>(gate));
    }

    if (GetModuleHandleW(loader))
    {
        return fail(L"a Vulkan loader of the private name was already present", 0);
    }
    wchar_t resolved[32768];
    const HRESULT loaded = query(resolved, _countof(resolved));
    if (FAILED(loaded))
    {
        return fail(L"private Vulkan loader did not load", static_cast<unsigned long>(loaded));
    }
    if (_wcsicmp(resolved, loader_path.c_str()))
    {
        std::fwprintf(stderr, L"resolved Vulkan loader: %ls\n", resolved);
        return fail(L"Vulkan loader is not the private sibling", 0);
    }
    std::wprintf(L"PASS %ls DXVK UMD: %ls -> %ls; OpenAdapter10 gate closed (hr=0x%08lx); Vulkan loader %ls\n",
                 arch, registered, runtime_path.c_str(), static_cast<unsigned long>(gate), resolved);
    return 0;
}
