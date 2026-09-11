// SPDX-License-Identifier: MIT
// Reuse the pinned Mesa raster/readback implementation without calling its
// application-local entrypoint (which loads opengl32.dll unconditionally).
#define main mesa_app_local_probe_main
#include "../../external/mesa/bin/windows-opengl-probe.cpp"
#undef main

int main(int argc, char **argv)
{
    if (argc != 3 ||
        (std::strcmp(argv[1], "--load-only") && std::strcmp(argv[1], "--gles1") && std::strcmp(argv[1], "--gles2")))
    {
        fail("usage: gles-probe --load-only|--gles1|--gles2 <installed payload directory>");
    }
#if defined(_M_ARM64)
    const char *architecture = "arm64";
#elif defined(_M_X64)
    const char *architecture = "x64";
#elif defined(_M_IX86)
    const char *architecture = "x86";
#else
#error Unsupported probe architecture
#endif
    char directory[MAX_PATH];
    const int length = std::snprintf(directory, sizeof(directory), "%s\\%s", argv[2], architecture);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(directory) || !SetCurrentDirectoryA(directory))
    {
        fail("installed architecture directory");
    }

    load(".\\z-1.dll");
    HMODULE vk = load(".\\vulkan-1.dll");
    HMODULE icd = load(".\\vulkan_freedreno.dll");
    load(".\\libgallium_wgl.dll");
    HMODULE egl = load(".\\libEGL.dll");
    HMODULE es1 = load(".\\libGLESv1_CM.dll");
    HMODULE es2 = load(".\\libGLESv2.dll");
    symbol<PROC>(vk, "vkGetInstanceProcAddr");
    symbol<PROC>(icd, "vk_icdGetInstanceProcAddr");
    symbol<PROC>(egl, "eglGetProcAddress");
    symbol<PROC>(es1, "glDrawArrays");
    symbol<PROC>(es2, "glCreateShader");

    if (!std::strcmp(argv[1], "--gles1"))
    {
        gles(egl, es1, 1);
    }
    else if (!std::strcmp(argv[1], "--gles2"))
    {
        gles(egl, es2, 2);
    }
    std::printf("PASS %s architecture=%s (%zu-bit process)\n", argv[1], architecture, sizeof(void *) * 8);
    return 0;
}
