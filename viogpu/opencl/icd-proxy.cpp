// SPDX-License-Identifier: MIT
// ARM64X ICD entry adapter only. Real CLVK dispatch tables stay in the matching
// native ARM64 or emulated x64 backend; this is not a merged compute runtime.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <CL/cl.h>
#include <cwchar>
#include <cstring>

static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static HMODULE backend;

static BOOL CALLBACK load_backend(PINIT_ONCE, PVOID, PVOID*) {
#if defined(_M_ARM64EC) || defined(_M_X64)
    const wchar_t* name = L"viogpucl_x64.dll";
#elif defined(_M_ARM64)
    const wchar_t* name = L"viogpucl_arm64.dll";
#elif defined(_M_IX86)
    const wchar_t* name = L"viogpucl_x86.dll";
#else
#error Unsupported OpenCL ICD architecture
#endif
    auto* storage = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, 65536 * sizeof(wchar_t)));
    if (!storage) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    wchar_t* path = storage;
    wchar_t* actual = storage + 32768;
    HMODULE self = nullptr;
    BOOL ok = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(&once), &self);
    DWORD size = ok ? GetModuleFileNameW(self, path, 32768) : 0;
    wchar_t* slash = size && size < 32768 ? wcsrchr(path, L'\\') : nullptr;
    if (slash) {
        slash[1] = 0;
        ok = wcscat_s(path, 32768, name) == 0;
    } else ok = FALSE;
    HMODULE existing = ok ? GetModuleHandleW(name) : nullptr;
    if (existing) {
        size = GetModuleFileNameW(existing, actual, 32768);
        ok = size && size < 32768 && !_wcsicmp(path, actual);
        if (!ok) SetLastError(ERROR_INVALID_DLL);
    }
    HMODULE loaded = ok ? LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                                       LOAD_LIBRARY_SEARCH_SYSTEM32) : nullptr;
    if (loaded) {
        size = GetModuleFileNameW(loaded, actual, 32768);
        if (!size || size >= 32768 || _wcsicmp(path, actual)) {
            FreeLibrary(loaded);
            loaded = nullptr;
            SetLastError(ERROR_INVALID_DLL);
        }
    }
    DWORD error = GetLastError();
    HeapFree(GetProcessHeap(), 0, storage);
    SetLastError(error);
    backend = loaded; // Retain for all returned dispatch/function-pointer lifetimes.
    return loaded != nullptr;
}

template<typename T> static T entry(const char* name) {
    if (!InitOnceExecuteOnce(&once, load_backend, nullptr, nullptr)) return nullptr;
    FARPROC symbol = GetProcAddress(backend, name);
    T result;
    static_assert(sizeof(result) == sizeof(symbol), "function pointer ABI");
    std::memcpy(&result, &symbol, sizeof(result));
    return result;
}

extern "C" void* CL_API_CALL clGetExtensionFunctionAddress(const char* name) {
    auto function = entry<decltype(&clGetExtensionFunctionAddress)>("clGetExtensionFunctionAddress");
    return function && name ? function(name) : nullptr;
}

extern "C" void* CL_API_CALL clGetExtensionFunctionAddressForPlatform(cl_platform_id platform, const char* name) {
    auto function = entry<decltype(&clGetExtensionFunctionAddressForPlatform)>("clGetExtensionFunctionAddressForPlatform");
    return function && name ? function(platform, name) : nullptr;
}

extern "C" cl_int CL_API_CALL clIcdGetPlatformIDsKHR(cl_uint count, cl_platform_id* platforms, cl_uint* returned) {
    using Function = cl_int(CL_API_CALL*)(cl_uint, cl_platform_id*, cl_uint*);
    auto function = entry<Function>("clIcdGetPlatformIDsKHR");
    return function ? function(count, platforms, returned) : -1001;
}
