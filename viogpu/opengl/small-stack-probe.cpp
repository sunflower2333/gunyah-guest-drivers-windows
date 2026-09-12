// SPDX-License-Identifier: MIT
// Execute the actual installed ICD exports on a constrained worker stack.
// No adapter registration, Vulkan instance, graphics context or GPU workload.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

struct Probe
{
    BOOL(WINAPI *validate)(ULONG) = nullptr;
    VkResult(VKAPI_PTR *negotiate)(uint32_t *) = nullptr;
};

static DWORD WINAPI run(void *argument)
{
    const Probe *probe = static_cast<const Probe *>(argument);
    // Repeat to check the initialized path as well as the first lazy load.
    for (unsigned attempt = 0; attempt < 2; ++attempt)
    {
        if (probe->validate && !probe->validate(1))
        {
            return 10;
        }
        uint32_t version = 7;
        if (probe->negotiate &&
            (probe->negotiate(&version) != VK_SUCCESS || version < 1 || version > 7))
        {
            return 11;
        }
    }
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 3 || (wcscmp(argv[1], L"--vulkan") && wcscmp(argv[1], L"--opengl")))
    {
        std::fprintf(stderr, "usage: small-stack-probe --vulkan|--opengl <absolute ICD DLL path>\n");
        return 2;
    }
    HMODULE module = LoadLibraryExW(argv[2], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module)
    {
        std::fprintf(stderr, "FAIL load ICD Win32=%lu\n", GetLastError());
        return 3;
    }
    const bool vulkan = !wcscmp(argv[1], L"--vulkan");
    FARPROC symbol = GetProcAddress(module, vulkan ? "vk_icdNegotiateLoaderICDInterfaceVersion" : "DrvValidateVersion");
    if (!symbol)
    {
        std::fprintf(stderr, "FAIL ICD export Win32=%lu\n", GetLastError());
        FreeLibrary(module);
        return 4;
    }
    Probe probe;
    static_assert(sizeof(probe.validate) == sizeof(symbol) && sizeof(probe.negotiate) == sizeof(symbol), "export ABI");
    if (vulkan)
    {
        std::memcpy(&probe.negotiate, &symbol, sizeof(symbol));
    }
    else
    {
        std::memcpy(&probe.validate, &symbol, sizeof(symbol));
    }
    HANDLE worker = CreateThread(nullptr, 64 * 1024, run, &probe, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!worker)
    {
        std::fprintf(stderr, "FAIL create 64 KiB worker Win32=%lu\n", GetLastError());
        FreeLibrary(module);
        return 5;
    }
    if (WaitForSingleObject(worker, 15000) != WAIT_OBJECT_0)
    {
        std::fprintf(stderr, "FAIL ICD worker deadline\n");
        // Do not unload code while the worker may still execute it.
        TerminateProcess(GetCurrentProcess(), 124);
        return 124;
    }
    DWORD result = 1;
    if (!GetExitCodeThread(worker, &result))
    {
        result = 6;
    }
    CloseHandle(worker);
    FreeLibrary(module);
    if (result)
    {
        std::fprintf(stderr, "FAIL small-stack ICD result=%lu\n", result);
        return 1;
    }
    std::printf("PASS actual ICD %s on 64 KiB reserved worker stack; no GPU context\n", vulkan ? "Vulkan" : "OpenGL");
    return 0;
}
