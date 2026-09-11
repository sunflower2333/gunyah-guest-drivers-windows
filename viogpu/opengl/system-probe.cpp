// SPDX-License-Identifier: MIT
// --load-only never creates a graphics context. --system uses Microsoft's runtime.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <vulkan/vulkan_core.h>

static int error(const char *what)
{
    std::fprintf(stderr, "FAIL %s Win32=%lu\n", what, GetLastError());
    return 1;
}
int wmain(int argc, wchar_t **argv)
{
    if (argc != 3)
    {
        return error("usage: system-probe --load-only|--system <absolute package opengl directory>");
    }
    wchar_t path[32768];
    if (!GetFullPathNameW(argv[2], _countof(path), path, nullptr))
    {
        return error("package path");
    }
    if (wcscat_s(path, L"\\"))
    {
        return error("path length");
    }
#if defined(_M_IX86)
    const wchar_t *icd = L"viogpuopengl_x86.dll";
    const wchar_t *arch = L"x86";
#elif defined(_M_ARM64)
    const wchar_t *icd = L"viogpuopengl.dll";
    const wchar_t *arch = L"arm64";
#else
    const wchar_t *icd = L"viogpuopengl.dll";
    const wchar_t *arch = L"x64";
#endif
    if (!wcscmp(argv[1], L"--load-only"))
    {
        if (wcscat_s(path, icd))
        {
            return error("ICD path length");
        }
        HMODULE module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module)
        {
            return error("load system ICD proxy");
        }
        FARPROC symbol = GetProcAddress(module, "DrvValidateVersion");
        BOOL(WINAPI * validate)(ULONG) = nullptr;
        static_assert(sizeof(validate) == sizeof(symbol), "ABI");
        std::memcpy(&validate, &symbol, sizeof(validate));
        if (!validate || !validate(1))
        {
            return error("real DrvValidateVersion");
        }
        VkResult(VKAPI_PTR * negotiate)(uint32_t *) = nullptr;
        symbol = GetProcAddress(module, "vk_icdNegotiateLoaderICDInterfaceVersion");
        std::memcpy(&negotiate, &symbol, sizeof(negotiate));
        uint32_t version = 7;
        if (!negotiate || negotiate(&version) != VK_SUCCESS || version < 1 || version > 7)
        {
            return error("real Vulkan ICD interface negotiation");
        }
        std::printf("VULKAN_ICD_INTERFACE=%u\n", version);
    }
    else if (!wcscmp(argv[1], L"--system"))
    {
        // The import table references Windows opengl32.dll, never Mesa's app-local wrapper.
        HMODULE runtime = GetModuleHandleW(L"opengl32.dll");
        wchar_t actual[32768], expected[32768];
        if (!runtime || !GetModuleFileNameW(runtime, actual, _countof(actual)) ||
            !GetSystemDirectoryW(expected, _countof(expected)) || wcscat_s(expected, L"\\opengl32.dll") ||
            _wcsicmp(actual, expected))
        {
            return error("Microsoft system OpenGL runtime identity");
        }
        std::wprintf(L"SYSTEM_OPENGL=%ls\n", actual);
        WNDCLASSW wc = {};
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"VioGpuSystemIcdProbe";
        if (!RegisterClassW(&wc))
        {
            return error("window class");
        }
        HWND window = CreateWindowW(wc.lpszClassName,
                                    L"System ICD probe",
                                    WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                    0,
                                    0,
                                    160,
                                    160,
                                    nullptr,
                                    nullptr,
                                    wc.hInstance,
                                    nullptr);
        HDC dc = GetDC(window);
        PIXELFORMATDESCRIPTOR format = {};
        format.nSize = sizeof(format);
        format.nVersion = 1;
        format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        format.iPixelType = PFD_TYPE_RGBA;
        format.cColorBits = 32;
        int index = ChoosePixelFormat(dc, &format);
        if (!index || !SetPixelFormat(dc, index, &format))
        {
            return error("system pixel format");
        }
        HGLRC context = wglCreateContext(dc);
        if (!context || !wglMakeCurrent(dc, context))
        {
            return error("system WGL context");
        }
        const char *renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
        std::printf("GL_RENDERER=%s\n", renderer ? renderer : "null");
        if (!renderer || !std::strstr(renderer, "zink") || !std::strstr(renderer, "TURNIP"))
        {
            return error("accelerated renderer");
        }
        glViewport(0, 0, 64, 64);
        glClearColor(0, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glColor3f(1, 0, 0);
        glBegin(GL_TRIANGLES);
        glVertex2f(-1, -1);
        glVertex2f(1, -1);
        glVertex2f(0, 1);
        glEnd();
        glFinish();
        GLubyte pixel[4] = {};
        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        GLenum status = glGetError();
        std::printf("RGBA=%u,%u,%u,%u GL_ERROR=%u\n", pixel[0], pixel[1], pixel[2], pixel[3], status);
        if (status || pixel[0] < 240 || pixel[1] > 15 || pixel[2] > 15 || !SwapBuffers(dc))
        {
            return error("system raster/present");
        }
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(context);
        ReleaseDC(window, dc);
        DestroyWindow(window);
    }
    else
    {
        return error("unknown mode");
    }
    HMODULE gallium = GetModuleHandleW(L"libgallium_wgl.dll");
    wchar_t actual[32768], expected[32768];
    if (!GetFullPathNameW(argv[2], _countof(expected), expected, nullptr) || wcscat_s(expected, L"\\") ||
        wcscat_s(expected, arch) || wcscat_s(expected, L"\\libgallium_wgl.dll") || !gallium ||
        !GetModuleFileNameW(gallium, actual, _countof(actual)) || _wcsicmp(actual, expected))
    {
        return error("actual architecture Mesa ICD path");
    }
    std::wprintf(L"MESA_ICD=%ls\nPASS %ls architecture=%ls\n", actual, argv[1], arch);
    return 0;
}
