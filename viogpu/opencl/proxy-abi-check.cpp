// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <CL/cl.h>
#include <cstdio>
#include <cstring>
#include <filesystem>

int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 4) return 2;
    auto proxy = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
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
        extension("clDefinitelyMissingVIOGPU")) return 1;
    wchar_t actual[1024];
    if (!GetModuleFileNameW(backend, actual, 1024) || !std::filesystem::equivalent(actual, argv[2])) return 1;
    if (argc == 4) {
        // Fixture-only full loader dispatch. The genuine runtime path above
        // deliberately does not enumerate a nonexistent CI GPU.
        SetEnvironmentVariableW(L"OCL_ICD_FILENAMES", argv[1]);
        auto loader = LoadLibraryExW(argv[3], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        using Enumerate = cl_int(CL_API_CALL*)(cl_uint, cl_platform_id*, cl_uint*);
        using Info = cl_int(CL_API_CALL*)(cl_platform_id, cl_platform_info, size_t, void*, size_t*);
        auto enumerate = loader ? reinterpret_cast<Enumerate>(GetProcAddress(loader, "clGetPlatformIDs")) : nullptr;
        auto info = loader ? reinterpret_cast<Info>(GetProcAddress(loader, "clGetPlatformInfo")) : nullptr;
        cl_platform_id platforms[16]{}; cl_uint count = 0;
        if (!enumerate || !info || enumerate(16, platforms, &count) || !count || count > 16) return 1;
        bool found = false;
        for (cl_uint i = 0; i < count; ++i) {
            char name[128]{};
            if (info(platforms[i], CL_PLATFORM_NAME, sizeof(name), name, nullptr) == CL_SUCCESS &&
                !std::strcmp(name, "VIOGPU dispatch ABI fixture")) found = true;
        }
        if (!found) return 1;
    }
    std::printf("PASS exact flat ICD backend and %s dispatch ABI pointer_bits=%zu; no GPU execution\n",
                argc == 4 ? "Khronos fixture" : "real runtime extension", sizeof(void*) * 8);
    return 0;
}
