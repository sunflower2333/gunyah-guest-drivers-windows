// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <CL/cl.h>
#include <cstdio>
#include <cstring>
#include <filesystem>

int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 4) return 2;
    HANDLE token = nullptr; TOKEN_ELEVATION elevation{}; DWORD returned = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned);
        CloseHandle(token);
    }
    std::printf("ICD ABI pointer_bits=%zu elevated=%lu mode=%ls\n", sizeof(void*) * 8,
                elevation.TokenIsElevated, argc == 4 ? argv[3] : L"real-extension");
    auto proxy_path = std::filesystem::absolute(argv[1]).make_preferred();
    auto proxy = LoadLibraryExW(proxy_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!proxy) { std::printf("FAIL proxy load error=%lu\n", GetLastError()); return 1; }
    using Extension = void*(CL_API_CALL*)(const char*);
    auto extension = reinterpret_cast<Extension>(GetProcAddress(proxy, "clGetExtensionFunctionAddress"));
    void* enumerator = extension ? extension("clIcdGetPlatformIDsKHR") : nullptr;
    auto backend = GetModuleHandleW(std::filesystem::path(argv[2]).filename().c_str());
    if (argc == 4 && !wcscmp(argv[3], L"missing")) {
        if (enumerator) return 1;
        std::puts("PASS missing sibling fails closed"); return 0;
    }
    if (!backend || !enumerator || enumerator != reinterpret_cast<void*>(GetProcAddress(backend, "clIcdGetPlatformIDsKHR")) ||
        extension("clDefinitelyMissingVIOGPU")) {
        std::printf("FAIL backend=%p enumerator=%p error=%lu\n", static_cast<void*>(backend), enumerator, GetLastError()); return 1;
    }
    wchar_t actual[1024];
    if (!GetModuleFileNameW(backend, actual, 1024) || !std::filesystem::equivalent(actual, argv[2])) return 1;
    if (argc == 4) {
        // Fixture-only full loader dispatch. The genuine runtime path above
        // deliberately does not enumerate a nonexistent CI GPU.
        auto loader_path = std::filesystem::absolute(argv[3]).make_preferred();
        auto loader = LoadLibraryExW(loader_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        using Enumerate = cl_int(CL_API_CALL*)(cl_uint, cl_platform_id*, cl_uint*);
        using Info = cl_int(CL_API_CALL*)(cl_platform_id, cl_platform_info, size_t, void*, size_t*);
        auto enumerate = loader ? reinterpret_cast<Enumerate>(GetProcAddress(loader, "clGetPlatformIDs")) : nullptr;
        auto info = loader ? reinterpret_cast<Info>(GetProcAddress(loader, "clGetPlatformInfo")) : nullptr;
        cl_platform_id platforms[16]{}; cl_uint count = 0;
        int status = enumerate ? enumerate(16, platforms, &count) : -1001;
        if (!enumerate || !info || status || !count || count > 16) {
            std::printf("FAIL Khronos loader=%p enumerate=%p info=%p status=%d count=%u error=%lu\n",
                        static_cast<void*>(loader), reinterpret_cast<void*>(enumerate),
                        reinterpret_cast<void*>(info), status, count, GetLastError()); return 1;
        }
        bool found = false;
        for (cl_uint i = 0; i < count; ++i) {
            char name[128]{};
            if (info(platforms[i], CL_PLATFORM_NAME, sizeof(name), name, nullptr) == CL_SUCCESS &&
                !std::strcmp(name, "VIOGPU dispatch ABI fixture")) found = true;
        }
        if (!found) { std::printf("FAIL fixture absent among %u platforms\n", count); return 1; }
    }
    std::printf("PASS exact flat ICD backend and %s dispatch ABI pointer_bits=%zu; no GPU execution\n",
                argc == 4 ? "Khronos fixture" : "real runtime extension", sizeof(void*) * 8);
    return 0;
}
