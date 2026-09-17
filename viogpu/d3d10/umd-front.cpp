// SPDX-License-Identifier: MIT
// ARM64X entry for the Mesa D3D10/11 user-mode driver. No rendering code.
//
// UserModeDriverName is one path for every 64-bit process, and on ARM64 that
// includes emulated x64 processes, which cannot load the native ARM64 Mesa
// UMD. Each view of this DLL hands the adapter entry points to the Mesa build
// for its own architecture, loaded by exact sibling path: a pure export
// forwarder would resolve its target through the application's DLL search
// path, which never contains the DriverStore directory. 32-bit processes do
// not come here; they load viogpud3d_x86.dll through UserModeDriverNameWow.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <cwchar>

// Another UMD family reuses this entry by defining both target names before
// including this file (viogpu/dxvk/umd-front.cpp); the defaults are Mesa's.
#ifndef VIOGPU_UMD_NATIVE_TARGET
#define VIOGPU_UMD_NATIVE_TARGET L"viogpud3d.dll"
#endif
#ifndef VIOGPU_UMD_EC_TARGET
#define VIOGPU_UMD_EC_TARGET L"viogpud3d_x64.dll"
#endif

#if defined(_M_ARM64) && !defined(_M_ARM64EC)
static const wchar_t target_name[] = VIOGPU_UMD_NATIVE_TARGET;
#elif defined(_M_X64) || defined(_M_ARM64EC)
static const wchar_t target_name[] = VIOGPU_UMD_EC_TARGET;
#else
#error The D3D UMD entry has only ARM64 and ARM64EC views
#endif

static INIT_ONCE target_once = INIT_ONCE_STATIC_INIT;
static HMODULE target;

// The D3D runtime can call OpenAdapter on a thread with little stack left, so
// the two long-path buffers live on the heap.
struct ModulePaths
{
    wchar_t path[32768];
    wchar_t actual[32768];
};

struct ScopedModulePaths
{
    ModulePaths *value = static_cast<ModulePaths *>(HeapAlloc(GetProcessHeap(), 0, sizeof(ModulePaths)));
    ~ScopedModulePaths()
    {
        DWORD error = GetLastError();
        if (value)
        {
            HeapFree(GetProcessHeap(), 0, value);
        }
        SetLastError(error);
    }
};

static HMODULE load_sibling(const wchar_t *name)
{
    HMODULE self = nullptr;
    ScopedModulePaths storage;
    if (!storage.value)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    auto &path = storage.value->path;
    auto &actual = storage.value->actual;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&target_once),
                            &self))
    {
        return nullptr;
    }
    DWORD length = GetModuleFileNameW(self, path, _countof(path));
    if (!length || length >= _countof(path))
    {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return nullptr;
    }
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash)
    {
        SetLastError(ERROR_BAD_PATHNAME);
        return nullptr;
    }
    slash[1] = 0;
    if (wcscat_s(path, name))
    {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return nullptr;
    }
    // A module of the same basename from another package (an older DriverStore
    // copy, a hand-registered candidate) must not be adopted silently.
    HMODULE existing = GetModuleHandleW(name);
    if (existing)
    {
        length = GetModuleFileNameW(existing, actual, _countof(actual));
        if (!length || length >= _countof(actual) || _wcsicmp(path, actual))
        {
            SetLastError(ERROR_INVALID_DLL);
            return nullptr;
        }
    }
    HMODULE result = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!result)
    {
        return nullptr;
    }
    length = GetModuleFileNameW(result, actual, _countof(actual));
    if (!length || length >= _countof(actual) || _wcsicmp(path, actual))
    {
        FreeLibrary(result);
        SetLastError(ERROR_INVALID_DLL);
        return nullptr;
    }
    // Held for the process lifetime: the runtime keeps calling into the
    // function tables OpenAdapter fills in.
    return result;
}

static BOOL CALLBACK load_target(PINIT_ONCE, PVOID, PVOID *)
{
    target = load_sibling(target_name);
    return target != nullptr;
}

static HRESULT last_error_result(void)
{
    DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_MOD_NOT_FOUND);
}

// OpenAdapter10 and OpenAdapter10_2 both take one D3D10DDIARG_OPENADAPTER
// pointer. It stays opaque here so the entry does not depend on the WDK.
using OpenAdapterEntry = HRESULT(APIENTRY *)(void *);

static HRESULT open_adapter(const char *name, void *args)
{
    if (!InitOnceExecuteOnce(&target_once, load_target, nullptr, nullptr))
    {
        return last_error_result();
    }
    FARPROC symbol = GetProcAddress(target, name);
    if (!symbol)
    {
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }
    OpenAdapterEntry entry;
    static_assert(sizeof(entry) == sizeof(symbol), "function pointer ABI");
    std::memcpy(&entry, &symbol, sizeof(entry));
    return entry(args);
}

extern "C" HRESULT APIENTRY OpenAdapter10(void *args)
{
    return open_adapter("OpenAdapter10", args);
}

extern "C" HRESULT APIENTRY OpenAdapter10_2(void *args)
{
    return open_adapter("OpenAdapter10_2", args);
}

// Load check without an adapter: resolves the architecture-matched Mesa UMD
// exactly as OpenAdapter would and returns it. Used by the package ABI probe.
extern "C" HMODULE APIENTRY VioGpuD3DUmdTarget(void)
{
    if (!InitOnceExecuteOnce(&target_once, load_target, nullptr, nullptr))
    {
        return nullptr;
    }
    return target;
}
