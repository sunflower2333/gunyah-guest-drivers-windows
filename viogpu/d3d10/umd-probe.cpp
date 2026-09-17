// SPDX-License-Identifier: MIT
// Loads the D3D user-mode driver a process of this architecture is registered
// to load from a package directory, and proves the architecture-matched Mesa
// UMD behind it resolves with its adapter entry points. No adapter, no device:
// an ABI/load check only, not rendering.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <string>

#if defined(_M_ARM64)
static const wchar_t arch[] = L"arm64";
static const wchar_t registered[] = L"viogpud3dx.dll";
static const wchar_t runtime[] = L"viogpud3d.dll";
static const WORD runtime_machine = IMAGE_FILE_MACHINE_ARM64;
#elif defined(_M_X64)
static const wchar_t arch[] = L"x64";
static const wchar_t registered[] = L"viogpud3dx.dll";
static const wchar_t runtime[] = L"viogpud3d_x64.dll";
static const WORD runtime_machine = IMAGE_FILE_MACHINE_AMD64;
#elif defined(_M_IX86)
static const wchar_t arch[] = L"x86";
static const wchar_t registered[] = L"viogpud3d_x86.dll";
static const wchar_t runtime[] = L"viogpud3d_x86.dll";
static const WORD runtime_machine = IMAGE_FILE_MACHINE_I386;
#else
#error Unsupported probe architecture
#endif

static int fail(const wchar_t *what, DWORD error = GetLastError())
{
    std::fwprintf(stderr, L"FAIL %ls D3D UMD: %ls (error %lu)\n", arch, what, error);
    return 1;
}

static bool exports_adapter_entries(HMODULE module)
{
    return GetProcAddress(module, "OpenAdapter10") && GetProcAddress(module, "OpenAdapter10_2");
}

static WORD image_machine(HMODULE module)
{
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(reinterpret_cast<const BYTE *>(module) + dos->e_lfanew);
    return nt->FileHeader.Machine;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2)
    {
        std::fwprintf(stderr, L"usage: d3d-umd-probe-%ls.exe <package directory>\n", arch);
        return 2;
    }
    std::wstring root = argv[1];
    if (!root.empty() && root.back() != L'\\' && root.back() != L'/')
    {
        root += L'\\';
    }
    const std::wstring entry_path = root + registered;
    const std::wstring runtime_path = root + runtime;
    HMODULE entry = LoadLibraryExW(entry_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!entry)
    {
        return fail(L"registered entry does not load");
    }
    if (!exports_adapter_entries(entry))
    {
        return fail(L"registered entry lacks OpenAdapter10/OpenAdapter10_2");
    }
    HMODULE mesa = entry;
    if (std::wcscmp(registered, runtime))
    {
        auto target = reinterpret_cast<HMODULE(APIENTRY *)(void)>(GetProcAddress(entry, "VioGpuD3DUmdTarget"));
        if (!target)
        {
            return fail(L"ARM64X entry lacks its load check");
        }
        mesa = target();
        if (!mesa)
        {
            return fail(L"ARM64X entry could not load its Mesa UMD");
        }
    }
    wchar_t actual[32768];
    DWORD length = GetModuleFileNameW(mesa, actual, _countof(actual));
    if (!length || length >= _countof(actual) || _wcsicmp(actual, runtime_path.c_str()))
    {
        return fail(L"Mesa UMD resolved from the wrong path", 0);
    }
    if (image_machine(mesa) != runtime_machine)
    {
        return fail(L"Mesa UMD has the wrong architecture", image_machine(mesa));
    }
    if (!exports_adapter_entries(mesa))
    {
        return fail(L"Mesa UMD lacks OpenAdapter10/OpenAdapter10_2");
    }
    std::wprintf(L"PASS %ls D3D UMD: %ls -> %ls\n", arch, registered, actual);
    return 0;
}
